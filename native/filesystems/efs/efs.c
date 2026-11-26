/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent SGI EFS native reader. All I/O is partition-relative Common V2.
 * On-disk layout references: SGI IRIX efs(4), inode(4), directory format;
 * Linux v6.12 UAPI efs_fs_sb.h and the retained central EFS source baseline.
 * No Linux VFS code or external helper is linked. New implementation.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

enum { BB=512, DINODE=128, DIRECT=12, ROOT=2, MAX_NAME=255 };
enum { REG=0100000, DIR=0040000, LINK=0120000 };
typedef struct {
    uint32_t size, first, group, groups, inodes, magic, free_blocks, free_inodes;
    uint32_t bitmap, bitmap_bytes, replica;
    uint16_t dirty;
    char label[6];
} Geometry;
typedef struct { uint32_t physical, logical, count; } Extent;
typedef struct {
    uint32_t ino, bytes, count, type;
    uint8_t descriptors[DIRECT*8];
} Inode;
typedef struct {
    IfsFsHostRuntimeV2 host;
    Geometry g;
    uint8_t *bitmap;
    uintptr_t token;
    int writable,fault;
    uint64_t recovered;
} Session;
static Session session;
static uintptr_t generation;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void){return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire);}
static int leave(int rc){atomic_flag_clear_explicit(&gate,memory_order_release);return rc;}
static int live(void *v){return v && (uintptr_t)v==session.token && !session.fault;}
static uint32_t be16(const uint8_t *p){return ((uint32_t)p[0]<<8)|p[1];}
static uint32_t be24(const uint8_t *p){return (be16(p)<<8)|p[2];}
static uint32_t be32(const uint8_t *p){return (be16(p)<<16)|be16(p+2);}
static void put16(uint8_t *p,uint32_t n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void put32(uint8_t *p,uint32_t n){put16(p,n>>16);put16(p+2,n);}
static int cancelled(const IfsFsHostRuntimeV2 *h){return h->cancelled && h->cancelled(h->context);}
static int read_bytes(const IfsFsHostRuntimeV2 *h,uint64_t off,void *p,uint32_t n)
{return ifs_fs_host_read_bytes_v2(h,off,p,n);}

/* Original EFS used shift-only checksum; later IRIX rotates the high bit.
 * Interpret bytes explicitly and use unsigned arithmetic on every host. */
static uint32_t super_checksum(const uint8_t *p,int rotate)
{
    uint32_t sum=0;
    for(unsigned i=0;i<88;i+=2){
        sum^=be16(p+i);
        sum=(sum<<1)|(rotate?(sum>>31):0u);
    }
    return sum;
}
static int geometry(const IfsFsHostRuntimeV2 *h,Geometry *g)
{
    uint8_t p[BB];
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->volume_bytes<2u*BB)return IFS_FS_NOT_FOUND;
    int rc=read_bytes(h,BB,p,BB);
    if(rc)return rc;
    uint32_t magic=be32(p+28);
    if(magic!=0x72959u && magic!=0x7295au)return IFS_FS_NOT_FOUND;
    if(be32(p+88)!=super_checksum(p,1) &&
       (magic!=0x72959u || be32(p+88)!=super_checksum(p,0)))
        return IFS_FS_CORRUPT;
    *g=(Geometry){.size=be32(p),.first=be32(p+4),.group=be32(p+8),
        .inodes=be16(p+12),.groups=be16(p+18),.dirty=(uint16_t)be16(p+20),
        .magic=magic,.bitmap_bytes=be32(p+44),.free_blocks=be32(p+48),.free_inodes=be32(p+52),
        .bitmap=be32(p+56),.replica=be32(p+60)};
    memcpy(g->label,p+32,6);
    if(!g->bitmap)g->bitmap=2u;
    const uint64_t end=(uint64_t)g->first+(uint64_t)g->group*g->groups;
    const uint64_t bmend=(uint64_t)g->bitmap+(g->bitmap_bytes+511ull)/BB;
    if(g->size>0x1000000u || g->size>h->volume_bytes/BB ||
       g->first<3u || !g->groups || !g->inodes || g->inodes>=g->group ||
       end>g->size || g->bitmap<2u || bmend>h->volume_bytes/BB ||
       g->bitmap_bytes<(g->size+7u)/8u || g->bitmap_bytes>0x200000u ||
       (g->bitmap<end && bmend>g->first) ||
       (g->replica && (g->replica>=h->volume_bytes/BB || g->replica<end ||
                       (g->replica>=g->bitmap && g->replica<bmend))) ||
       g->free_blocks>(uint64_t)(g->group-g->inodes)*g->groups ||
       g->free_inodes>(uint64_t)g->inodes*g->groups*4u)
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
#include "journal.h"
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if(!variant)return IFS_FS_INVALID;
    *variant=0;Geometry g;
    int rc=geometry(h,&g);
    if(!rc)*variant=g.magic;
    return rc;
}
/* EFS has a native one-bit-per-basic-block bitmap. The cylinder group
 * inode tables, boot and superblock areas are outside the data allocator
 * even on foreign media where their bits are set. Never expose them as
 * free. Bit numbering is low-bit first within each byte. */
static int data_block(const Geometry *g,uint64_t b)
{
    if(b<g->first)return 0;
    uint64_t rel=b-g->first;
    return rel/g->group<g->groups && rel%g->group>=g->inodes;
}
static int free_bit(const uint8_t *bitmap,uint32_t b)
{return (bitmap[b/8u]&(uint8_t)(1u<<(b%8u)))!=0;}
static int load_bitmap(Session *s)
{
    const Geometry *g=&s->g;const IfsFsHostRuntimeV2 *h=&s->host;
    uint8_t *p=h->allocate(h->context,g->bitmap_bytes);
    if(!p)return IFS_FS_NO_MEMORY;
    int rc=read_bytes(h,(uint64_t)g->bitmap*BB,p,g->bitmap_bytes);
    uint64_t free=0;
    if(!rc)for(uint32_t cg=0;cg<g->groups;++cg){
        if(cancelled(h)){rc=IFS_FS_BUSY;break;}
        uint32_t start=g->first+cg*g->group+g->inodes;
        uint32_t end=g->first+(cg+1u)*g->group;
        for(uint32_t b=start;b<end;++b)free+=(unsigned)free_bit(p,b);
    }
    if(!rc && free!=g->free_blocks)rc=IFS_FS_CORRUPT;
    if(rc){h->release(h->context,p);return rc;}
    s->bitmap=p;return IFS_FS_OK;
}
static int inode_raw(const Session *s,uint32_t ino,uint8_t p[DINODE])
{
    const Geometry *g=&s->g;
    const uint32_t per=g->inodes*4u;
    if(ino<ROOT || ino/per>=g->groups)return IFS_FS_CORRUPT;
    uint64_t block=(uint64_t)g->first+(uint64_t)(ino/per)*g->group+(ino%per)/4u;
    return read_bytes(&s->host,block*BB+(ino%4u)*DINODE,p,DINODE);
}
static int inode_decode(uint32_t ino,const uint8_t p[DINODE],Inode *out)
{
    if(!be16(p+2))return IFS_FS_CORRUPT;
    const uint32_t mode=be16(p)&0170000u;
    uint32_t type;
    switch(mode){
    case REG:type=IFS_FS_ENTRY_FILE;break;
    case DIR:type=IFS_FS_ENTRY_DIRECTORY;break;
    case LINK:type=IFS_FS_ENTRY_LINK;break;
    default:return IFS_FS_UNSUPPORTED;
    }
    *out=(Inode){.ino=ino,.bytes=be32(p+8),.count=be16(p+28),.type=type};
    memcpy(out->descriptors,p+32,sizeof out->descriptors);
    if(out->bytes>INT32_MAX || out->count>32767u ||
       (type==IFS_FS_ENTRY_DIRECTORY && (!out->bytes || out->bytes%BB)))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int inode_load(const Session *s,uint32_t ino,Inode *out)
{
    uint8_t p[DINODE];int rc=inode_raw(s,ino,p);
    return rc?rc:inode_decode(ino,p,out);
}
static int decode_extent(const Session *s,const uint8_t *p,Extent *e)
{
    const Geometry *g=&s->g;
    *e=(Extent){.physical=be24(p+1),.logical=be24(p+5),.count=p[4]};
    if(p[0] || !e->count || e->physical<g->first)return IFS_FS_CORRUPT;
    uint32_t at=e->physical-g->first,group=at/g->group,within=at%g->group;
    if(group>=g->groups || within<g->inodes || e->count>g->group-within)
        return IFS_FS_CORRUPT;
    for(uint32_t i=0;s->bitmap && i<e->count;++i)
        if(free_bit(s->bitmap,e->physical+i))return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
/* Mark actual physical blocks of the object, including indirect tables.
 * This catches aliased extents within an inode before returning payloads.
 * The complete filesystem ownership check is a separate future operation. */
static int claim(uint8_t *seen,const Extent *e)
{
    for(uint32_t i=0;i<e->count;++i){
        uint32_t b=e->physical+i;uint8_t mask=(uint8_t)(1u<<(b%8u));
        if(seen[b/8u]&mask)return IFS_FS_CORRUPT;
        seen[b/8u]|=mask;
    }
    return IFS_FS_OK;
}
static int extents_claim(const Session *s,const Inode *ino,uint8_t *global,Extent **result)
{
    const IfsFsHostRuntimeV2 *h=&s->host;
    *result=NULL;
    if(!ino->bytes)return ino->count?IFS_FS_CORRUPT:IFS_FS_OK;
    if(!ino->count)return ino->type==IFS_FS_ENTRY_LINK?IFS_FS_UNSUPPORTED:IFS_FS_CORRUPT;
    Extent *out=h->allocate(h->context,ino->count*sizeof(*out));
    uint8_t *seen=global?global:h->allocate(h->context,(s->g.size+7u)/8u);
    int rc=IFS_FS_NO_MEMORY;
    if(!out || !seen)goto done;
    if(!global)memset(seen,0,(s->g.size+7u)/8u);
    uint32_t filled=0;
    if(ino->count<=DIRECT){
        for(;filled<ino->count;++filled){
            rc=decode_extent(s,ino->descriptors+8u*filled,&out[filled]);
            if(rc)goto done;
        }
    }else{
        /* The first indirect descriptor's logical field stores the
         * number of inode-resident descriptors pointing at extent tables. */
        const uint32_t tables=be24(ino->descriptors+5);
        if(!tables || tables>DIRECT){rc=IFS_FS_CORRUPT;goto done;}
        for(uint32_t i=0;i<tables;++i){
            Extent table;
            rc=decode_extent(s,ino->descriptors+8u*i,&table);
            if(rc)goto done;
            rc=claim(seen,&table);if(rc)goto done;
            for(uint32_t j=0;j<table.count && filled<ino->count;++j){
                uint8_t p[BB];
                if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
                rc=read_bytes(h,(uint64_t)(table.physical+j)*BB,p,BB);
                if(rc)goto done;
                for(uint32_t k=0;k<BB/8u && filled<ino->count;++k,++filled){
                    rc=decode_extent(s,p+8u*k,&out[filled]);
                    if(rc)goto done;
                }
            }
        }
    }
    if(filled!=ino->count){rc=IFS_FS_CORRUPT;goto done;}
    uint32_t cursor=0,need=(ino->bytes+511u)/BB;
    for(uint32_t i=0;i<filled;++i){
        if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
        if(out[i].logical!=cursor || out[i].count>need-cursor){
            rc=IFS_FS_CORRUPT;goto done;
        }
        rc=claim(seen,&out[i]);if(rc)goto done;
        cursor+=out[i].count;
    }
    if(cursor!=need){rc=IFS_FS_CORRUPT;goto done;}
    *result=out;out=NULL;rc=IFS_FS_OK;
done:
    if(out)h->release(h->context,out);
    if(seen && !global)h->release(h->context,seen);
    return rc;
}
static int extents(const Session *s,const Inode *ino,Extent **out)
{return extents_claim(s,ino,NULL,out);}
static int read_inode(const Session *s,const Inode *ino,uint64_t at,void *data,uint32_t n)
{
    if((!data && n) || at>ino->bytes || n>ino->bytes-at)return IFS_FS_INVALID;
    Extent *ext=NULL;int rc=extents(s,ino,&ext);
    if(rc)return rc;
    uint8_t *out=data;
    for(uint32_t i=0;i<ino->count && n;++i){
        uint64_t start=(uint64_t)ext[i].logical*BB,end=start+(uint64_t)ext[i].count*BB;
        if(at>=end)continue;
        if(at<start){rc=IFS_FS_CORRUPT;break;}
        uint32_t take=n;
        if(end-at<take)take=(uint32_t)(end-at);
        if(cancelled(&s->host)){rc=IFS_FS_BUSY;break;}
        rc=read_bytes(&s->host,(uint64_t)ext[i].physical*BB+at-start,out,take);
        if(rc)break;
        at+=take;out+=take;n-=take;
    }
    if(!rc && n)rc=IFS_FS_CORRUPT;
    if(ext)s->host.release(s->host.context,ext);
    return rc;
}
typedef struct { uint32_t ino,len; char name[MAX_NAME+1]; } Dirent;
typedef int (*DirVisit)(void *,const Dirent *);
static int dot_kind(const Dirent *e)
{
    if(e->len==1u && e->name[0]=='.')return 1;
    if(e->len==2u && e->name[0]=='.' && e->name[1]=='.')return 2;
    return 0;
}
/* Validate every slot in every directory block. Reuse the resolved native
 * extent inventory for the complete walk rather than reparsing it per block.
 * A visitor sees dot entries too; ordinary namespace enumeration hides them. */
static int directory_walk(const Session *s,const Inode *dir,DirVisit visit,void *ctx)
{
    if(dir->type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_INVALID;
    Extent *ext=NULL;int rc=extents(s,dir,&ext);if(rc)return rc;
    for(uint32_t i=0;i<dir->count && !rc;++i)
        for(uint32_t k=0;k<ext[i].count && !rc;++k){
            if(cancelled(&s->host)){rc=IFS_FS_BUSY;break;}
            uint8_t p[BB],used[BB]={0};
            rc=read_bytes(&s->host,(uint64_t)(ext[i].physical+k)*BB,p,BB);
            if(rc)break;
            if(be16(p)!=0xbeefu){rc=IFS_FS_CORRUPT;break;}
            const uint32_t slots=p[3],header=4u+slots;
            for(uint32_t slot=0;slot<slots;++slot){
                uint32_t off=2u*p[4u+slot];
                if(!off)continue;
                if(off<header || off>BB-5u){rc=IFS_FS_CORRUPT;break;}
                uint32_t ino=be32(p+off),n=p[off+4];
                if(!n || n>BB-off-5u || ino<ROOT ||
                   ino/(s->g.inodes*4u)>=s->g.groups){rc=IFS_FS_CORRUPT;break;}
                for(uint32_t j=off;j<off+5u+n;++j){
                    if(used[j]){rc=IFS_FS_CORRUPT;break;}
                    used[j]=1;
                }
                if(rc)break;
                Dirent e={.ino=ino,.len=n};
                memcpy(e.name,p+off+5u,n);
                for(uint32_t j=0;j<n;++j)
                    if(!e.name[j] || e.name[j]=='/'){rc=IFS_FS_CORRUPT;break;}
                if(rc)break;
                rc=visit(ctx,&e);if(rc)break;
            }
        }
    if(ext)s->host.release(s->host.context,ext);
    return rc;
}
typedef struct { const char *name;uint32_t len,index,visible,found;Dirent *out; } Find;
static int find_visit(void *ctx,const Dirent *e)
{
    Find *f=ctx;
    if(dot_kind(e))return IFS_FS_OK;
    int match=f->name?(f->len==e->len && !memcmp(f->name,e->name,e->len)):
        f->visible==f->index;
    ++f->visible;
    if(match){
        if(f->found)return IFS_FS_CORRUPT;
        *f->out=*e;f->found=1;
    }
    return IFS_FS_OK;
}
static int directory_find(const Session *s,const Inode *dir,const char *name,
                          uint32_t len,uint32_t index,Dirent *out)
{
    Find f={.name=name,.len=len,.index=index,.out=out};
    int rc=directory_walk(s,dir,find_visit,&f);
    if(rc)return rc;
    return f.found?IFS_FS_OK:name?IFS_FS_NOT_FOUND:IFS_FS_END;
}
static int resolve(const Session *s,const char *path,Inode *ino,Dirent *ent)
{
    if(!path || *path!='/')return IFS_FS_INVALID;
    int rc=inode_load(s,ROOT,ino);if(rc)return rc;
    *ent=(Dirent){.ino=ROOT,.len=1,.name="/"};
    ++path;
    for(unsigned depth=0;*path;++depth){
        if(depth>=256)return IFS_FS_INVALID;
        uint32_t n=0;while(path[n] && path[n]!='/' && n<=MAX_NAME)++n;
        if(!n || n>MAX_NAME || (n==1 && path[0]=='.') ||
           (n==2 && path[0]=='.' && path[1]=='.'))return IFS_FS_INVALID;
        rc=directory_find(s,ino,path,n,0,ent);if(rc)return rc;
        rc=inode_load(s,ent->ino,ino);if(rc)return rc;
        path+=n;
        if(*path=='/'){++path;if(!*path)return IFS_FS_INVALID;}
    }
    return IFS_FS_OK;
}
static void volume_info(IfsFsVolumeInfoV2 *o)
{
    *o=(IfsFsVolumeInfoV2){.structure_bytes=sizeof *o,.variant=session.g.magic,
       .max_name_bytes=MAX_NAME,.block_bytes=BB,
       .total_bytes=(uint64_t)session.g.size*BB,
       .free_bytes=(uint64_t)session.g.free_blocks*BB};
}
static int check_native(const Session *s);
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o)
{
    if(!v || !o || o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    *v=NULL;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(session.token || generation==UINTPTR_MAX)return IFS_FS_BUSY;
    const int writable=!(h->capabilities&IFS_FS_HOST_READ_ONLY);
    Geometry g;int rc=geometry(h,&g);
    uint64_t recovered=0;
    /* Foreign EFS remains readable, but mutation requires a journal that
     * was installed by this provider's native format. A torn primary may
     * be replayed only from a validated prepared journal on a writable host. */
    Journal j;int jr=IFS_FS_NOT_FOUND;
    if(journal_supported(h) && (rc || g.first==journal_first(h)))
        jr=journal_load(h,&j);
    if(!jr && j.count){
        if(!writable)return IFS_FS_BUSY;
        int rr=journal_replay(h,&j);if(rr)return rr;
        recovered=1;rc=geometry(h,&g);
    }
    if(rc)return rc;
    if(g.dirty)return IFS_FS_UNSUPPORTED;
    if(journal_supported(h) && g.first==journal_first(h) && jr!=IFS_FS_OK)
        return jr==IFS_FS_NOT_FOUND?IFS_FS_CORRUPT:jr;
    if(writable && (jr || g.first!=journal_first(h)))return IFS_FS_ACCESS;
    session=(Session){.g=g,.writable=writable,.recovered=recovered};
    size_t n=h->structure_bytes<sizeof(session.host)?h->structure_bytes:sizeof(session.host);
    memcpy(&session.host,h,n);session.host.structure_bytes=(uint32_t)n;
    Inode root;Dirent dummy;
    rc=load_bitmap(&session);
    if(!rc)rc=inode_load(&session,ROOT,&root);
    if(!rc && root.type!=IFS_FS_ENTRY_DIRECTORY)rc=IFS_FS_CORRUPT;
    if(!rc){rc=directory_find(&session,&root,NULL,0,UINT32_MAX,&dummy);if(rc==IFS_FS_END)rc=IFS_FS_OK;}
    if(!rc && writable)rc=check_native(&session);
    if(rc){
        if(session.bitmap)h->release(h->context,session.bitmap);
        session=(Session){0};return rc;
    }
    session.token=++generation;*v=(void *)session.token;volume_info(o);return IFS_FS_OK;
}
static int unmount_impl(void *v)
{
    if(!v || (uintptr_t)v!=session.token)return IFS_FS_INVALID;
    session.host.release(session.host.context,session.bitmap);
    session=(Session){0};return IFS_FS_OK;
}
static int fill_entry(const Inode *ino,const Dirent *ent,IfsFsEntryV2 *o)
{
    if(!o || o->structure_bytes<sizeof *o || !o->name)return IFS_FS_INVALID;
    o->name_bytes=0;
    if(o->name_capacity<=ent->len)return IFS_FS_NO_SPACE;
    memcpy(o->name,ent->name,ent->len+1u);
    o->name_bytes=ent->len;o->size_bytes=ino->bytes;o->type=ino->type;o->attributes=0;
    return IFS_FS_OK;
}
static int stat_impl(void *v,const char *path,IfsFsEntryV2 *o)
{
    if(!live(v))return IFS_FS_INVALID;
    Inode ino;Dirent ent;int rc=resolve(&session,path,&ino,&ent);
    return rc?rc:fill_entry(&ino,&ent,o);
}
static int list_impl(void *v,const char *path,uint32_t index,IfsFsEntryV2 *o)
{
    if(!live(v))return IFS_FS_INVALID;
    Inode dir,ino;Dirent ent;
    int rc=resolve(&session,path,&dir,&ent);
    if(!rc)rc=directory_find(&session,&dir,NULL,0,index,&ent);
    if(!rc)rc=inode_load(&session,ent.ino,&ino);
    return rc?rc:fill_entry(&ino,&ent,o);
}
static int read_impl(void *v,const char *path,uint64_t at,void *p,uint32_t n)
{
    if(!live(v))return IFS_FS_INVALID;
    Inode ino;Dirent ent;int rc=resolve(&session,path,&ino,&ent);
    if(rc)return rc;
    if(ino.type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    return read_inode(&session,&ino,at,p,n);
}
static int map_impl(void *v,const char *path,uint64_t first,IfsFsExtentV2 *out,uint32_t cap,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0;
    if(!live(v)||!out||!cap)return IFS_FS_INVALID;
    Inode ino;Dirent ent;int rc=resolve(&session,path,&ino,&ent);
    if(rc)return rc;
    Extent *ext=NULL;rc=extents(&session,&ino,&ext);if(rc)return rc;
    /* Merge native extents only when logically and physically adjacent.
     * MAP cursors count the returned coalesced extents, not disk records. */
    uint64_t index=0;uint32_t count=0;
    for(uint32_t i=0;i<ino.count;){
        Extent e=ext[i++];
        while(i<ino.count && e.physical+e.count==ext[i].physical &&
              e.logical+e.count==ext[i].logical)e.count+=ext[i++].count;
        if(index++<first)continue;
        if(count==cap)break;
        uint64_t logical=(uint64_t)e.logical*BB,length=(uint64_t)e.count*BB;
        if(length>ino.bytes-logical)length=ino.bytes-logical;
        out[count++]=(IfsFsExtentV2){.logical_offset=logical,
            .physical_offset=(uint64_t)e.physical*BB,.length=length};
    }
    if(ext)session.host.release(session.host.context,ext);
    *written=count;return count?IFS_FS_OK:IFS_FS_END;
}
/* Complete partition-relative allocation map from the validated native
 * bitmap and cylinder-group geometry. The tail beyond the EFS data area,
 * including a replica superblock, remains reserved. */
static int allocation_impl(void *v,uint64_t first,IfsFsAllocationRangeV2 *out,
                           uint32_t capacity,uint32_t *written)
{
    if(!live(v)||!out||!capacity||capacity>64u||first>session.host.volume_bytes)
        return IFS_FS_INVALID;
    if(first==session.host.volume_bytes)return IFS_FS_END;
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint64_t at=first;uint32_t count=0,scanned=0;
    while(at<session.host.volume_bytes && scanned<65536u){
        if(cancelled(&session.host))return IFS_FS_BUSY;
        uint64_t b=at/BB,end=(b+1u)*BB;
        if(end>session.host.volume_bytes)end=session.host.volume_bytes;
        uint32_t state=IFS_FS_SPACE_RESERVED;
        if(data_block(&session.g,b))
            state=free_bit(session.bitmap,(uint32_t)b)?IFS_FS_SPACE_FREE:IFS_FS_SPACE_ALLOCATED;
        if(count && page[count-1u].state==state)page[count-1u].length+=end-at;
        else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){.physical_offset=at,.length=end-at,.state=state};
        }
        at=end;++scanned;
    }
    memcpy(out,page,count*sizeof(*page));*written=count;return IFS_FS_OK;
}
static int allocation_map(void *v,uint64_t first,IfsFsAllocationRangeV2 *out,
                          uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0;
    if(!enter())return IFS_FS_BUSY;
    return leave(allocation_impl(v,first,out,capacity,written));
}

#define FORMAT_MIN (UINT64_C(1024)*1024u)
#define FORMAT_MAX (UINT64_C(0x1000000)*BB)
static int write_bytes(const IfsFsHostRuntimeV2 *h,uint64_t off,const void *p,uint32_t n)
{return ifs_fs_host_write_bytes_v2(h,off,p,n);}
static int flush(const IfsFsHostRuntimeV2 *h)
{return h->flush(h->context)?IFS_FS_IO_ERROR:IFS_FS_OK;}
static void set_free(uint8_t *p,uint32_t b)
{p[b/8u]|=(uint8_t)(1u<<(b%8u));}
static void make_dir_entry(uint8_t *p,uint32_t slot,uint32_t offset,const char *name,uint32_t n)
{
    p[2]=(uint8_t)(offset/2u);p[3]=(uint8_t)(slot+1u);
    p[4u+slot]=(uint8_t)(offset/2u);
    put32(p+offset,ROOT);p[offset+4u]=(uint8_t)n;memcpy(p+offset+5u,name,n);
}
/* Format a conventional uniform-cylinder-group EFS partition with a
 * native free bitmap and primary/replica superblocks. Invalidate the
 * primary before modifying metadata and publish it only after the root,
 * bitmap, inode tables and replica reach durable storage. This is
 * destructive formatting, not a transaction-recovery implementation. */
static int format_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_ACCESS;
    if(session.token)return IFS_FS_BUSY;
    if(variant!=0x72959u || flags || h->volume_bytes<FORMAT_MIN ||
       h->volume_bytes>FORMAT_MAX || !journal_supported(h))return IFS_FS_UNSUPPORTED;
    uint32_t label_len=0;
    if(label)while(label_len<7u && label[label_len]){
        unsigned char c=(unsigned char)label[label_len];
        if(c<32u||c>126u)return IFS_FS_INVALID;
        ++label_len;
    }
    if(label_len>6u)return IFS_FS_INVALID;
    uint32_t total=(uint32_t)(h->volume_bytes/BB),bmsize=(total+7u)/8u;
    uint32_t bmblocks=(bmsize+511u)/BB,first=journal_first(h);
    uint32_t usable=total-first-1u,groups=(usable+65535u)/65536u;
    uint32_t group=usable/groups,inodes=group/32u;
    if(!inodes)inodes=1u;
    uint32_t end=first+groups*group,root=first+inodes;
    uint8_t *bitmap=h->allocate(h->context,(size_t)bmblocks*BB);
    if(!bitmap)return IFS_FS_NO_MEMORY;
    memset(bitmap,0,(size_t)bmblocks*BB);
    for(uint32_t cg=0;cg<groups;++cg)
        for(uint32_t b=first+cg*group+inodes;b<first+(cg+1u)*group;++b)
            if(b!=root)set_free(bitmap,b);
    uint8_t p[BB]={0},sb[BB]={0};
    int rc=cancelled(h)?IFS_FS_BUSY:write_bytes(h,BB,p,BB);
    if(!rc)rc=flush(h);
    if(rc)goto done;
    rc=journal_init(h);if(rc)goto done;
    for(uint32_t cg=0;cg<groups;++cg){
        for(uint32_t i=0;i<inodes;++i){
            if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
            memset(p,0,BB);
            if(!cg && !i){
                uint8_t *inode=p+ROOT*DINODE;
                put16(inode,0040755u);put16(inode+2,2u);
                put32(inode+8,BB);put16(inode+28,1u);
                inode[33]=(uint8_t)(root>>16);inode[34]=(uint8_t)(root>>8);
                inode[35]=(uint8_t)root;inode[36]=1u;
            }
            rc=write_bytes(h,(uint64_t)(first+cg*group+i)*BB,p,BB);
            if(rc)goto done;
        }
    }
    memset(p,0,BB);put16(p,0xbeefu);
    make_dir_entry(p,0u,504u,".",1u);
    make_dir_entry(p,1u,496u,"..",2u);
    rc=write_bytes(h,(uint64_t)root*BB,p,BB);if(rc)goto done;
    rc=write_bytes(h,2u*BB,bitmap,bmblocks*BB);if(rc)goto done;
    put32(sb,end);put32(sb+4,first);put32(sb+8,group);
    put16(sb+12,inodes);put16(sb+14,63u);put16(sb+16,1u);put16(sb+18,groups);
    put32(sb+28,0x72959u);
    if(label_len){memcpy(sb+32,label,label_len);memcpy(sb+38,label,label_len);}
    put32(sb+44,bmsize);put32(sb+48,groups*(group-inodes)-1u);
    put32(sb+52,groups*inodes*4u-3u);
    put32(sb+60,total-1u);put32(sb+64,ROOT);
    put32(sb+88,super_checksum(sb,1));
    rc=flush(h);if(rc)goto done;
    rc=write_bytes(h,(uint64_t)(total-1u)*BB,sb,BB);if(rc)goto done;
    rc=flush(h);if(rc)goto done;
    if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
    rc=write_bytes(h,BB,sb,BB);
    if(!rc)rc=flush(h);
    if(rc){
        memset(p,0,BB);
        (void)write_bytes(h,BB,p,BB);(void)flush(h);goto done;
    }
    Geometry g;rc=geometry(h,&g);
done:
    h->release(h->context,bitmap);return rc;
}
static int format_profile(IfsFsFormatProfileV2 *p)
{
    if(!p||p->structure_bytes<sizeof(*p))return IFS_FS_INVALID;
    *p=(IfsFsFormatProfileV2){.structure_bytes=sizeof *p,.flags=IFS_FS_FORMAT_ACCEPTS_LABEL|IFS_FS_FORMAT_REQUIRE_CHECK,
        .format_variant=0x72959u,.probe_variant=0x72959u,.exact_block_bytes=BB,
        .layout=IFS_FS_LAYOUT_MULTI_EXTENT,.minimum_bytes=FORMAT_MIN,
        .recommended_bytes=16u*1024u*1024u,.maximum_bytes=FORMAT_MAX};
    return IFS_FS_OK;
}
/* Full independent integrity pass: reconcile every live inode with
 * the allocator, count native link references, validate directory parent
 * relationships and connectivity, and reject crosslinked or orphan blocks.
 * It never repairs, clears a dirty flag, or writes any storage. */
typedef struct { uint32_t type,links,refs,parent,dotdot,mark; } CheckNode;
typedef struct { const Session *s;CheckNode *nodes;uint32_t total,current;
    uint32_t count,capacity,dot,dotdot;Dirent *names; } CheckDirectory;
static int compare_names(const void *a,const void *b)
{return strcmp(((const Dirent*)a)->name,((const Dirent*)b)->name);}
static int check_visit(void *ctx,const Dirent *e)
{
    CheckDirectory *c=ctx;
    if(e->ino>=c->total || !c->nodes[e->ino].type ||
       c->nodes[e->ino].refs==UINT16_MAX)return IFS_FS_CORRUPT;
    ++c->nodes[e->ino].refs;
    int dot=dot_kind(e);
    if(dot==1){
        if(c->dot++ || e->ino!=c->current)return IFS_FS_CORRUPT;
    }else if(dot==2){
        if(c->dotdot++ || c->nodes[e->ino].type!=DIR)return IFS_FS_CORRUPT;
        c->nodes[c->current].dotdot=e->ino;
    }else if(c->nodes[e->ino].type==DIR){
        if(c->nodes[e->ino].parent)return IFS_FS_CORRUPT;
        c->nodes[e->ino].parent=c->current;
    }
    if(c->count==c->capacity){
        uint32_t next=c->capacity?c->capacity*2u:16u;
        if(next<c->capacity || next>UINT32_MAX/sizeof(Dirent))return IFS_FS_NO_MEMORY;
        const IfsFsHostRuntimeV2 *h=&c->s->host;
        Dirent *p=h->allocate(h->context,(size_t)next*sizeof(*p));
        if(!p)return IFS_FS_NO_MEMORY;
        if(c->names){
            memcpy(p,c->names,(size_t)c->count*sizeof(*p));
            h->release(h->context,c->names);
        }
        c->names=p;c->capacity=next;
    }
    c->names[c->count++]=*e;
    return IFS_FS_OK;
}
static int check_native(const Session *s)
{
    const IfsFsHostRuntimeV2 *h=&s->host;
    const Geometry *g=&s->g;
    uint64_t count64=(uint64_t)g->inodes*g->groups*4u;
    if(count64>SIZE_MAX/sizeof(CheckNode) || count64>UINT32_MAX)return IFS_FS_NO_MEMORY;
    uint32_t total=(uint32_t)count64;
    CheckNode *nodes=h->allocate(h->context,(size_t)total*sizeof(*nodes));
    uint8_t *claimed=h->allocate(h->context,(g->size+7u)/8u);
    int rc=IFS_FS_NO_MEMORY;
    if(!nodes||!claimed)goto done;
    memset(nodes,0,(size_t)total*sizeof(*nodes));
    memset(claimed,0,(g->size+7u)/8u);
    uint32_t free_inodes=0;
    for(uint32_t i=ROOT;i<total;++i){
        if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
        uint8_t p[DINODE];
        rc=inode_raw(s,i,p);if(rc)goto done;
        if(!be16(p)){++free_inodes;continue;}
        uint32_t mode=be16(p)&0170000u;
        uint32_t links=be16(p+2);
        if(!links){rc=IFS_FS_CORRUPT;goto done;}
        nodes[i].type=mode;nodes[i].links=links;
        if(mode==REG||mode==DIR||mode==LINK){
            Inode ino;Extent *ext=NULL;
            rc=inode_decode(i,p,&ino);if(rc)goto done;
            /* Inline symlink payload occupies the inode address field,
             * not a data block. An out-of-line link uses normal extents. */
            if(mode==LINK && !ino.count && ino.bytes<=sizeof(ino.descriptors))continue;
            rc=extents_claim(s,&ino,claimed,&ext);
            if(ext)h->release(h->context,ext);
            if(rc)goto done;
        }else if(mode==0010000u||mode==0020000u||mode==0060000u||mode==0140000u){
            /* FIFO, character/block device, or socket: native special
             * inode with no filesystem data extents. */
            if(be32(p+8)||be16(p+28)){rc=IFS_FS_CORRUPT;goto done;}
        }else {rc=IFS_FS_UNSUPPORTED;goto done;}
    }
    if(free_inodes!=g->free_inodes||nodes[ROOT].type!=DIR){rc=IFS_FS_CORRUPT;goto done;}
    nodes[ROOT].parent=ROOT;
    for(uint32_t i=ROOT;i<total;++i){
        if(nodes[i].type!=DIR)continue;
        Inode dir;rc=inode_load(s,i,&dir);if(rc)goto done;
        CheckDirectory c={.s=s,.nodes=nodes,.total=total,.current=i};
        rc=directory_walk(s,&dir,check_visit,&c);
        if(!rc && (c.dot!=1u||c.dotdot!=1u))rc=IFS_FS_CORRUPT;
        if(!rc){
            qsort(c.names,c.count,sizeof(*c.names),compare_names);
            for(uint32_t k=1;k<c.count;++k)
                if(!strcmp(c.names[k-1].name,c.names[k].name)){rc=IFS_FS_CORRUPT;break;}
        }
        if(c.names)h->release(h->context,c.names);
        if(rc)goto done;
    }
    for(uint32_t i=ROOT;i<total;++i){
        CheckNode *n=&nodes[i];
        if(!n->type)continue;
        if(!n->refs||n->refs!=n->links ||
           (n->type==DIR && (!n->parent||n->parent!=n->dotdot))){
            rc=IFS_FS_CORRUPT;goto done;
        }
    }
    /* Parent graph must terminate at root. Mark each path once to avoid
     * a quadratic chain walk even on a deeply nested volume. */
    nodes[ROOT].mark=2u;
    for(uint32_t i=ROOT+1u;i<total;++i){
        if(nodes[i].type!=DIR||nodes[i].mark==2u)continue;
        uint32_t k=i;
        while(nodes[k].mark==0u){nodes[k].mark=1u;k=nodes[k].parent;}
        if(nodes[k].mark!=2u){rc=IFS_FS_CORRUPT;goto done;}
        k=i;
        while(nodes[k].mark==1u){nodes[k].mark=2u;k=nodes[k].parent;}
    }
    /* Only actual allocatable data blocks participate in bitmap/owner
     * reconciliation. Inode-table bits may be set on authentic IRIX media. */
    for(uint32_t cg=0;cg<g->groups;++cg){
        if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
        uint32_t start=g->first+cg*g->group+g->inodes,end=g->first+(cg+1u)*g->group;
        for(uint32_t k=start;k<end;++k)
            if(free_bit(s->bitmap,k)==free_bit(claimed,k)){rc=IFS_FS_CORRUPT;goto done;}
    }
    rc=IFS_FS_OK;
done:
    if(nodes)h->release(h->context,nodes);
    if(claimed)h->release(h->context,claimed);
    return rc;
}
static int check_impl(void *v,uint32_t flags,uint64_t *errors)
{
    if(!errors)return IFS_FS_INVALID;
    *errors=0;
    if(!live(v)||flags)return IFS_FS_INVALID;
    Session current={.host=session.host};
    int rc=geometry(&current.host,&current.g);
    if(!rc && current.g.dirty)rc=IFS_FS_CORRUPT;
    if(!rc)rc=load_bitmap(&current);
    if(!rc)rc=check_native(&current);
    if(current.bitmap)current.host.release(current.host.context,current.bitmap);
    if(rc==IFS_FS_CORRUPT){*errors=1u;return IFS_FS_OK;}
    return rc;
}
static int query_impl(void *v,IfsFsVolumeInfoV2 *o)
{
    if(!live(v)||!o||o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    volume_info(o);return IFS_FS_OK;
}
/* All future writable namespace changes must stage the native superblock,
 * replica and metadata in this same transaction before committing. */
static int stage_super(Tx *t,const uint8_t sb[BB],uint32_t replica)
{
    int rc=tx_stage(t,BB,sb,BB);
    if(!rc && replica)rc=tx_stage(t,(uint64_t)replica*BB,sb,BB);
    return rc;
}
#include "mutation.h"
static int set_label_impl(void *v,const char *name)
{
    if(!live(v)||!name)return IFS_FS_INVALID;
    if(!session.writable)return IFS_FS_ACCESS;
    uint32_t n=0;
    while(n<7u && name[n]){
        unsigned char c=(unsigned char)name[n];
        if(c<32u||c>126u)return IFS_FS_INVALID;
        ++n;
    }
    if(n>6u)return IFS_FS_INVALID;
    if(cancelled(&session.host))return IFS_FS_BUSY;
    uint8_t sb[BB];int rc=read_bytes(&session.host,BB,sb,BB);if(rc)return rc;
    memset(sb+32,0,12);memcpy(sb+32,name,n);memcpy(sb+38,name,n);
    put32(sb+88,super_checksum(sb,1));
    Tx t;rc=tx_begin(&t,&session.host);if(rc)return rc;
    rc=stage_super(&t,sb,session.g.replica);
    if(!rc && cancelled(&session.host))rc=IFS_FS_BUSY;
    if(!rc){
        rc=tx_commit(&t);
        if(rc)session.fault=1;
    }
    tx_end(&t);
    if(!rc)memcpy(session.g.label,sb+32,6);
    return rc;
}
static int sync_impl(void *v)
{
    if(!live(v))return IFS_FS_INVALID;
    return session.writable?jflush(&session.host):IFS_FS_OK;
}
static int recover_impl(void *v,uint32_t flags,uint64_t *repaired)
{
    if(!repaired)return IFS_FS_INVALID;
    *repaired=0;
    if(!live(v)||flags)return IFS_FS_INVALID;
    if(!session.writable)return IFS_FS_ACCESS;
    *repaired=session.recovered;session.recovered=0;
    return IFS_FS_OK;
}
static int label_impl(void *v,char *p,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;
    if(!live(v)||!p)return IFS_FS_INVALID;
    uint32_t len=0;while(len<6u && session.g.label[len])++len;
    if(cap<=len)return IFS_FS_NO_SPACE;
    memcpy(p,session.g.label,len);p[len]=0;*n=len;return IFS_FS_OK;
}
#define WRAP(name,args,call) static int name args {if(!enter())return IFS_FS_BUSY;return leave(call);}
WRAP(mount,(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o),mount_impl(h,v,o))
WRAP(unmount,(void *v),unmount_impl(v))
WRAP(stat_path,(void *v,const char *p,IfsFsEntryV2 *o),stat_impl(v,p,o))
WRAP(list_path,(void *v,const char *p,uint32_t i,IfsFsEntryV2 *o),list_impl(v,p,i,o))
WRAP(read_path,(void *v,const char *p,uint64_t at,void *b,uint32_t n),read_impl(v,p,at,b,n))
WRAP(write_path,(void *v,const char *p,uint64_t at,const void *b,uint32_t n),write_impl(v,p,at,b,n))
WRAP(create_path,(void *v,const char *p,uint32_t type),create_impl(v,p,type))
WRAP(remove_path,(void *v,const char *p),remove_impl(v,p))
WRAP(rename_path,(void *v,const char *p,const char *q),rename_impl(v,p,q))
WRAP(truncate_path,(void *v,const char *p,uint64_t n),truncate_impl(v,p,n))
WRAP(query_volume,(void *v,IfsFsVolumeInfoV2 *o),query_impl(v,o))
WRAP(check,(void *v,uint32_t flags,uint64_t *errors),check_impl(v,flags,errors))
WRAP(label,(void *v,char *p,uint32_t c,uint32_t *n),label_impl(v,p,c,n))
WRAP(set_label,(void *v,const char *name),set_label_impl(v,name))
WRAP(sync_volume,(void *v),sync_impl(v))
WRAP(recover,(void *v,uint32_t flags,uint64_t *n),recover_impl(v,flags,n))
WRAP(format,(const IfsFsHostRuntimeV2 *h,uint32_t variant,const char *name,uint32_t flags),format_impl(h,variant,name,flags))
static int map_path(void *v,const char *p,uint64_t first,IfsFsExtentV2 *out,uint32_t c,uint32_t *n)
{
    if(n)*n=0;
    if(!enter())return IFS_FS_BUSY;
    return leave(map_impl(v,p,first,out,c,n));
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof core,IFS_FS_GROUP_CORE,0,
        IFS_FS_CORE_ALL},
    .probe=probe,.mount=mount,.unmount=unmount,.stat=stat_path,.directory_entry=list_path,.read=read_path,
    .write=write_path,.create=create_path,.remove=remove_path,.rename=rename_path,.truncate=truncate_path};
static const IfsFsVolumeOpsV2 volume={
    .header={IFS_FS_GROUP_ABI_V2,sizeof volume,IFS_FS_GROUP_VOLUME,0,
        IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET|IFS_FS_VOLUME_FORMAT|IFS_FS_VOLUME_CHECK|
        IFS_FS_VOLUME_LABEL_SET|IFS_FS_VOLUME_SYNC},
    .query=query_volume,.label=label,.format=format,.check=check,.set_label=set_label,.sync=sync_volume};
static const IfsFsAllocationOpsV2 allocation={
    .header={IFS_FS_GROUP_ABI_V2,sizeof allocation,IFS_FS_GROUP_ALLOCATION,0,IFS_FS_ALLOCATION_MAP},
    .map=allocation_map};
static const IfsFsFormatProfileOpsV2 profile={
    .header={IFS_FS_GROUP_ABI_V2,sizeof profile,IFS_FS_GROUP_FORMAT_PROFILE,0,IFS_FS_FORMAT_PROFILE_DEFAULT},
    .get_default=format_profile};
static const IfsFsDefragOpsV2 defrag={
    .header={IFS_FS_GROUP_ABI_V2,sizeof defrag,IFS_FS_GROUP_DEFRAG,0,IFS_FS_DEFRAG_MAP|IFS_FS_DEFRAG_RECOVER},
    .map=map_path,.recover=recover};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{
    if(lo>2u||hi<2u)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation.header;
    if(id==IFS_FS_GROUP_FORMAT_PROFILE)return &profile.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))static const char identity[]="IFS-MODULE:efs:common2";
static const IfsFsModuleApiV2 api={
    .abi_version=2,.structure_bytes=sizeof api,.format_id="efs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_FORMAT_PROFILE),.query_group=group};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
