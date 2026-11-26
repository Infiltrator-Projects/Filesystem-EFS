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

enum { BB=512, DINODE=128, DIRECT=12, ROOT=2, MAX_NAME=255 };
enum { REG=0100000, DIR=0040000, LINK=0120000 };
typedef struct {
    uint32_t size, first, group, groups, inodes, magic, free_blocks;
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
    uintptr_t token;
} Session;
static Session session;
static uintptr_t generation;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void){return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire);}
static int leave(int rc){atomic_flag_clear_explicit(&gate,memory_order_release);return rc;}
static int live(void *v){return v && (uintptr_t)v==session.token;}
static uint32_t be16(const uint8_t *p){return ((uint32_t)p[0]<<8)|p[1];}
static uint32_t be24(const uint8_t *p){return (be16(p)<<8)|p[2];}
static uint32_t be32(const uint8_t *p){return (be16(p)<<16)|be16(p+2);}
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
        .magic=magic,.bitmap_bytes=be32(p+44),.free_blocks=be32(p+48),
        .bitmap=be32(p+56),.replica=be32(p+60)};
    memcpy(g->label,p+32,6);
    if(!g->bitmap)g->bitmap=2u;
    const uint64_t end=(uint64_t)g->first+(uint64_t)g->group*g->groups;
    const uint64_t bmend=(uint64_t)g->bitmap+(g->bitmap_bytes+511ull)/BB;
    if(g->size>0x1000000u || g->size>h->volume_bytes/BB ||
       g->first<3u || !g->groups || !g->inodes || g->inodes>=g->group ||
       end>g->size || g->bitmap<2u || bmend>g->size ||
       g->bitmap_bytes<(g->size+7u)/8u ||
       (g->bitmap<end && bmend>g->first) ||
       (g->replica && (g->replica>=g->size || g->replica<end)) ||
       g->free_blocks>(uint64_t)(g->group-g->inodes)*g->groups ||
       be32(p+52)>(uint64_t)g->inodes*g->groups*4u)
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if(!variant)return IFS_FS_INVALID;
    *variant=0;Geometry g;
    int rc=geometry(h,&g);
    if(!rc)*variant=g.magic;
    return rc;
}
static int inode_load(const Session *s,uint32_t ino,Inode *out)
{
    const Geometry *g=&s->g;
    const uint32_t per=g->inodes*4u;
    if(ino<ROOT || ino/per>=g->groups)return IFS_FS_CORRUPT;
    uint64_t block=(uint64_t)g->first+(uint64_t)(ino/per)*g->group+(ino%per)/4u;
    uint8_t p[DINODE];
    int rc=read_bytes(&s->host,block*BB+(ino%4u)*DINODE,p,DINODE);
    if(rc)return rc;
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
static int decode_extent(const Session *s,const uint8_t *p,Extent *e)
{
    const Geometry *g=&s->g;
    *e=(Extent){.physical=be24(p+1),.logical=be24(p+5),.count=p[4]};
    if(p[0] || !e->count || e->physical<g->first)return IFS_FS_CORRUPT;
    uint32_t at=e->physical-g->first,group=at/g->group,within=at%g->group;
    if(group>=g->groups || within<g->inodes || e->count>g->group-within)
        return IFS_FS_CORRUPT;
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
static int extents(const Session *s,const Inode *ino,Extent **result)
{
    const IfsFsHostRuntimeV2 *h=&s->host;
    *result=NULL;
    if(!ino->bytes)return ino->count?IFS_FS_CORRUPT:IFS_FS_OK;
    if(!ino->count)return ino->type==IFS_FS_ENTRY_LINK?IFS_FS_UNSUPPORTED:IFS_FS_CORRUPT;
    Extent *out=h->allocate(h->context,ino->count*sizeof(*out));
    uint8_t *seen=h->allocate(h->context,(s->g.size+7u)/8u);
    int rc=IFS_FS_NO_MEMORY;
    if(!out || !seen)goto done;
    memset(seen,0,(s->g.size+7u)/8u);
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
    if(seen)h->release(h->context,seen);
    return rc;
}
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
/* Directory slots point to 2-byte-aligned entries in their 512-byte
 * containing block. Validate every slot before exposing any name. */
static int directory_find(const Session *s,const Inode *dir,const char *name,
                          uint32_t len,uint32_t index,Dirent *out)
{
    if(dir->type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_INVALID;
    uint32_t visible=0,found=0;
    for(uint32_t at=0;at<dir->bytes;at+=BB){
        uint8_t p[BB],used[BB]={0};
        int rc=read_inode(s,dir,at,p,BB);if(rc)return rc;
        if(be16(p)!=0xbeefu)return IFS_FS_CORRUPT;
        const uint32_t slots=p[3],header=4u+slots;
        for(uint32_t slot=0;slot<slots;++slot){
            uint32_t off=2u*p[4u+slot];
            if(!off)continue;
            if(off<header || off>BB-5u)return IFS_FS_CORRUPT;
            uint32_t ino=be32(p+off),n=p[off+4];
            if(!n || n>BB-off-5u || ino<ROOT ||
               ino/(s->g.inodes*4u)>=s->g.groups)return IFS_FS_CORRUPT;
            for(uint32_t j=off;j<off+5u+n;++j){
                if(used[j])return IFS_FS_CORRUPT;
                used[j]=1;
            }
            const char *entry=(const char *)p+off+5u;
            for(uint32_t j=0;j<n;++j)if(!entry[j] || entry[j]=='/')return IFS_FS_CORRUPT;
            if((n==1 && entry[0]=='.') ||
               (n==2 && entry[0]=='.' && entry[1]=='.'))continue;
            int match=name?(len==n && !memcmp(name,entry,n)):(visible==index);
            ++visible;
            if(match){
                if(found)return IFS_FS_CORRUPT;
                *out=(Dirent){.ino=ino,.len=n};
                memcpy(out->name,entry,n);found=1;
            }
        }
    }
    return found?IFS_FS_OK:name?IFS_FS_NOT_FOUND:IFS_FS_END;
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
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o)
{
    if(!v || !o || o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    *v=NULL;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(!(h->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    if(session.token || generation==UINTPTR_MAX)return IFS_FS_BUSY;
    Geometry g;int rc=geometry(h,&g);if(rc)return rc;
    if(g.dirty)return IFS_FS_UNSUPPORTED;
    session=(Session){.g=g};
    size_t n=h->structure_bytes<sizeof(session.host)?h->structure_bytes:sizeof(session.host);
    memcpy(&session.host,h,n);session.host.structure_bytes=(uint32_t)n;
    Inode root;Dirent dummy;
    rc=inode_load(&session,ROOT,&root);
    if(!rc && root.type!=IFS_FS_ENTRY_DIRECTORY)rc=IFS_FS_CORRUPT;
    if(!rc){rc=directory_find(&session,&root,NULL,0,UINT32_MAX,&dummy);if(rc==IFS_FS_END)rc=IFS_FS_OK;}
    if(rc){session=(Session){0};return rc;}
    session.token=++generation;*v=(void *)session.token;volume_info(o);return IFS_FS_OK;
}
static int unmount_impl(void *v)
{
    if(!live(v))return IFS_FS_INVALID;
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
static int query_impl(void *v,IfsFsVolumeInfoV2 *o)
{
    if(!live(v)||!o||o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    volume_info(o);return IFS_FS_OK;
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
WRAP(query_volume,(void *v,IfsFsVolumeInfoV2 *o),query_impl(v,o))
WRAP(label,(void *v,char *p,uint32_t c,uint32_t *n),label_impl(v,p,c,n))
static int map_path(void *v,const char *p,uint64_t first,IfsFsExtentV2 *out,uint32_t c,uint32_t *n)
{
    if(n)*n=0;
    if(!enter())return IFS_FS_BUSY;
    return leave(map_impl(v,p,first,out,c,n));
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof core,IFS_FS_GROUP_CORE,0,
        IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=probe,.mount=mount,.unmount=unmount,.stat=stat_path,.directory_entry=list_path,.read=read_path};
static const IfsFsVolumeOpsV2 volume={
    .header={IFS_FS_GROUP_ABI_V2,sizeof volume,IFS_FS_GROUP_VOLUME,0,
        IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET},.query=query_volume,.label=label};
static const IfsFsDefragOpsV2 defrag={
    .header={IFS_FS_GROUP_ABI_V2,sizeof defrag,IFS_FS_GROUP_DEFRAG,0,IFS_FS_DEFRAG_MAP},
    .map=map_path};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{
    if(lo>2u||hi<2u)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))static const char identity[]="IFS-MODULE:efs:common2";
static const IfsFsModuleApiV2 api={
    .abi_version=2,.structure_bytes=sizeof api,.format_id="efs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),.query_group=group};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
