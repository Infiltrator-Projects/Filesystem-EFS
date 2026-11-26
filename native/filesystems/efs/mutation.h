/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native EFS copy-on-write namespace mutation, included by efs.c.
 * Data is allocated using the original and prospective native bitmaps.
 * Old blocks cannot be reused until the transaction commits. Only pages
 * containing exclusively previously-free data may be written ahead of the
 * journal; all mixed pages and metadata use full-page redo.
 */
typedef struct {
    Tx tx;
    uint8_t *bitmap;
    uint32_t free_blocks,free_inodes;
} Mutation;
typedef struct { Extent *e;uint32_t count,capacity; } Runs;
typedef struct { Dirent *e;uint32_t count,capacity; } Names;
typedef struct { Inode old;uint8_t raw[DINODE]; } Object;
static const IfsFsHostRuntimeV2 *mh(void){return &session.host;}
static void *mem_new(size_t n){return mh()->allocate(mh()->context,n);}
static void mem_drop(void *p){if(p)mh()->release(mh()->context,p);}
static void runs_drop(Runs *r){mem_drop(r->e);*r=(Runs){0};}
static void names_drop(Names *n){mem_drop(n->e);*n=(Names){0};}
static int mutation_begin(Mutation *m,void *v)
{
    *m=(Mutation){0};
    if(!live(v))return IFS_FS_INVALID;
    if(!session.writable)return IFS_FS_ACCESS;
    if(active_plan && !plan_executing)return IFS_FS_BUSY;
    if(cancelled(mh()))return IFS_FS_BUSY;
    int rc=tx_begin(&m->tx,mh());if(rc)return rc;
    m->bitmap=mem_new(session.g.bitmap_bytes);
    if(!m->bitmap){tx_end(&m->tx);return IFS_FS_NO_MEMORY;}
    memcpy(m->bitmap,session.bitmap,session.g.bitmap_bytes);
    m->free_blocks=session.g.free_blocks;m->free_inodes=session.g.free_inodes;
    return IFS_FS_OK;
}
static void mutation_end(Mutation *m)
{mem_drop(m->bitmap);m->bitmap=NULL;tx_end(&m->tx);}
static int mutation_commit(Mutation *m)
{
    uint8_t sb[BB];int rc=read_bytes(mh(),BB,sb,BB);if(rc)return rc;
    if(be32(sb+88)!=super_checksum(sb,1))return IFS_FS_CORRUPT;
    put32(sb+48,m->free_blocks);put32(sb+52,m->free_inodes);
    put32(sb+88,super_checksum(sb,1));
    for(uint32_t at=0;at<session.g.bitmap_bytes;at+=BB){
        uint32_t n=session.g.bitmap_bytes-at;if(n>BB)n=BB;
        if(memcmp(m->bitmap+at,session.bitmap+at,n)){
            rc=tx_stage(&m->tx,(uint64_t)session.g.bitmap*BB+at,m->bitmap+at,n);
            if(rc)return rc;
        }
    }
    rc=stage_super(&m->tx,sb,session.g.replica);
    if(rc)return rc;
    if(cancelled(mh()))return IFS_FS_BUSY;
    rc=tx_commit(&m->tx);
    if(rc){session.fault=1;return rc;}
    memcpy(session.bitmap,m->bitmap,session.g.bitmap_bytes);
    session.g.free_blocks=m->free_blocks;session.g.free_inodes=m->free_inodes;
    rc=reservations_reload(&session);
    if(rc)session.fault=1;
    return rc;
}
static int mark_allocated(Mutation *m,uint32_t b)
{
    if(!data_block(&session.g,b)||!free_bit(session.bitmap,b)||
       !free_bit(m->bitmap,b)||!m->free_blocks)return IFS_FS_CORRUPT;
    m->bitmap[b/8u]&=(uint8_t)~(1u<<(b%8u));--m->free_blocks;
    return IFS_FS_OK;
}
static int release_extent(Mutation *m,const Extent *e)
{
    for(uint32_t i=0;i<e->count;i++){
        uint32_t b=e->physical+i;
        if(!data_block(&session.g,b)||free_bit(session.bitmap,b)||
           free_bit(m->bitmap,b))return IFS_FS_CORRUPT;
        set_free(m->bitmap,b);++m->free_blocks;
    }
    return IFS_FS_OK;
}
static int allocate_runs(Mutation *m,uint32_t blocks,Runs *r)
{
    *r=(Runs){0};
    if(!blocks)return IFS_FS_OK;
    if(blocks>m->free_blocks)return IFS_FS_NO_SPACE;
    r->capacity=blocks<32767u?blocks:32767u;
    r->e=mem_new((size_t)r->capacity*sizeof(*r->e));
    if(!r->e)return IFS_FS_NO_MEMORY;
    uint32_t logical=0;
    for(uint32_t cg=0;cg<session.g.groups && logical<blocks;cg++){
        uint32_t b=session.g.first+cg*session.g.group+session.g.inodes;
        uint32_t end=session.g.first+(cg+1u)*session.g.group;
        for(;b<end && logical<blocks;b++){
            if((b&4095u)==0 && cancelled(mh()))return IFS_FS_BUSY;
            if(!free_bit(session.bitmap,b)||!free_bit(m->bitmap,b)||
               (plan_executing&&plan_avoid&&free_bit(plan_avoid,b)))continue;
            Extent *last=r->count?&r->e[r->count-1u]:NULL;
            if(last && last->count<255u && last->physical+last->count==b &&
               (last->physical-session.g.first)/session.g.group==cg)++last->count;
            else {
                if(r->count==r->capacity)return IFS_FS_NO_SPACE;
                r->e[r->count++]=(Extent){.physical=b,.logical=logical,.count=1};
            }
            int rc=mark_allocated(m,b);if(rc)return rc;
            ++logical;
        }
    }
    return logical==blocks?IFS_FS_OK:IFS_FS_NO_SPACE;
}
/* Exact contiguous native run; split only at the 255-block record limit. */
static int exact_runs(Mutation *m,uint32_t start,uint32_t blocks,int allocate,Runs *r)
{
    *r=(Runs){0};
    if(!blocks)return IFS_FS_OK;
    if(!data_block(&session.g,start)||blocks>session.g.size-start||
       blocks>session.g.group-(start-session.g.first)%session.g.group)
        return IFS_FS_INVALID;
    r->capacity=(blocks+254u)/255u;
    if(r->capacity>32767u)return IFS_FS_NO_SPACE;
    r->e=mem_new((size_t)r->capacity*sizeof(*r->e));if(!r->e)return IFS_FS_NO_MEMORY;
    for(uint32_t i=0;i<blocks;i++){
        uint32_t b=start+i;
        if(allocate){int rc=mark_allocated(m,b);if(rc)return rc;}
        else if(free_bit(session.bitmap,b)||free_bit(m->bitmap,b))return IFS_FS_CORRUPT;
        if(!i||r->e[r->count-1u].count==255u)
            r->e[r->count++]=(Extent){.physical=b,.logical=i,.count=1};
        else ++r->e[r->count-1u].count;
    }
    return IFS_FS_OK;
}
static void encode_extent(uint8_t *p,const Extent *e,uint32_t logical)
{
    p[0]=0;p[1]=(uint8_t)(e->physical>>16);p[2]=(uint8_t)(e->physical>>8);
    p[3]=(uint8_t)e->physical;p[4]=(uint8_t)e->count;
    p[5]=(uint8_t)(logical>>16);p[6]=(uint8_t)(logical>>8);p[7]=(uint8_t)logical;
}
/* Free only data owned by the original inode, including its native
 * indirect tables. The allocator will not reuse these blocks this tx. */
static int release_tables(Mutation *m,const Inode *ino);
static int release_old(Mutation *m,const Inode *ino,const Extent *old)
{
    int rc;
    for(uint32_t i=0;i<ino->count;i++){
        rc=release_extent(m,&old[i]);if(rc)return rc;
    }
    return release_tables(m,ino);
}
static int release_tables(Mutation *m,const Inode *ino)
{
    int rc;
    if(ino->count>DIRECT){
        uint32_t tables=be24(ino->descriptors+5);
        for(uint32_t i=0;i<tables;i++){
            Extent e;rc=decode_extent(&session,ino->descriptors+8u*i,&e);
            if(rc)return rc;
            rc=release_extent(m,&e);if(rc)return rc;
        }
    }
    return IFS_FS_OK;
}
static uint64_t inode_offset(uint32_t ino)
{
    uint32_t per=session.g.inodes*4u;
    uint64_t b=(uint64_t)session.g.first+(uint64_t)(ino/per)*session.g.group+(ino%per)/4u;
    return b*BB+(ino%4u)*DINODE;
}
static int stage_inode(Mutation *m,uint32_t ino,const uint8_t p[DINODE])
{return tx_stage(&m->tx,inode_offset(ino),p,DINODE);}
static int load_object(uint32_t ino,Object *o)
{
    int rc=inode_raw(&session,ino,o->raw);
    return rc?rc:inode_decode(ino,o->raw,&o->old);
}
static int new_inode(Mutation *m,uint32_t type,Object *o)
{
    if(!m->free_inodes)return IFS_FS_NO_SPACE;
    uint32_t total=session.g.inodes*session.g.groups*4u;
    uint8_t raw[DINODE];int found=0;uint32_t ino=ROOT+1u;
    for(;ino<total;ino++){
        if((ino&255u)==0 && cancelled(mh()))return IFS_FS_BUSY;
        int rc=inode_raw(&session,ino,raw);if(rc)return rc;
        if(!be16(raw)){found=1;break;}
    }
    if(!found)return IFS_FS_CORRUPT;
    *o=(Object){.old={.ino=ino,.type=type}};
    put16(o->raw,type==IFS_FS_ENTRY_DIRECTORY?0040755u:0100644u);
    put16(o->raw+2,type==IFS_FS_ENTRY_DIRECTORY?2u:1u);
    uint32_t gen=be32(raw+24);
    put32(o->raw+24,gen==UINT32_MAX?1u:gen+1u);
    --m->free_inodes;
    return IFS_FS_OK;
}
static void stamp(uint8_t p[DINODE],int created)
{
    const IfsFsHostRuntimeV2 *h=mh();
    if(!h->realtime_ns)return;
    uint64_t sec=h->realtime_ns(h->context)/UINT64_C(1000000000);
    uint32_t time=sec>UINT32_MAX?UINT32_MAX:(uint32_t)sec;
    if(created)put32(p+12,time);
    put32(p+16,time);put32(p+20,time);
}
/* A failed early write cannot damage a live file, even on a 4Kn device.
 * Mixed physical pages (including bitmap, inodes and neighbouring files)
 * are never written before a durable journal header. */
static int write_new_block(Mutation *m,uint32_t block,const uint8_t p[BB])
{
    if(!free_bit(session.bitmap,block)||free_bit(m->bitmap,block))
        return IFS_FS_CORRUPT;
    uint32_t page=block/(JUNIT/BB);
    uint32_t first=page*(JUNIT/BB);
    int private_page=1;
    for(uint32_t i=0;i<JUNIT/BB;i++)
        if(!data_block(&session.g,first+i)||!free_bit(session.bitmap,first+i))
            {private_page=0;break;}
    for(uint32_t i=0;i<m->tx.count;i++)if(m->tx.page[i]==page)private_page=0;
    if(!private_page)return tx_stage(&m->tx,(uint64_t)block*BB,p,BB);
    uint8_t full[JUNIT];
    int rc=jread(mh(),(uint64_t)page*JUNIT,full);if(rc)return rc;
    memcpy(full+(block-first)*BB,p,BB);
    return jwrite(mh(),(uint64_t)page*JUNIT,full);
}
/* Build the canonical 8-byte EFS extent records. If more than twelve
 * records are needed, the inode points at native indirect extent blocks.
 * No artificial side-table or consumer-specific storage format is used. */
static int build_descriptors(Mutation *m,Object *o,const Runs *data)
{
    memset(o->raw+32,0,DIRECT*8u);put16(o->raw+28,data->count);
    if(data->count<=DIRECT){
        for(uint32_t i=0;i<data->count;i++)
            encode_extent(o->raw+32+8u*i,&data->e[i],data->e[i].logical);
        return IFS_FS_OK;
    }
    Runs tables;int rc=allocate_runs(m,(data->count+63u)/64u,&tables);
    if(!rc && tables.count>DIRECT)rc=IFS_FS_NO_SPACE;
    if(!rc){
        uint32_t index=0;
        for(uint32_t i=0;i<tables.count && !rc;i++){
            const Extent *e=&tables.e[i];
            encode_extent(o->raw+32+8u*i,e,i?0u:tables.count);
            for(uint32_t j=0;j<e->count;j++){
                uint8_t p[BB]={0};
                for(uint32_t k=0;k<64u && index<data->count;k++,index++)
                    encode_extent(p+8u*k,&data->e[index],data->e[index].logical);
                rc=write_new_block(m,e->physical+j,p);if(rc)break;
            }
        }
        if(!rc && index!=data->count)rc=IFS_FS_CORRUPT;
    }
    runs_drop(&tables);return rc;
}
typedef int (*BlockSource)(void *,uint32_t,uint8_t[BB]);
/* Replace a stream with freshly allocated native blocks, then atomically
 * switch the inode and allocator in the redo transaction. The source
 * supplies full zero-padded blocks and may retain the old payload. */
static int replace_stream(Mutation *m,Object *o,uint32_t bytes,BlockSource source,void *ctx)
{
    Extent *old=NULL;
    int rc=extents(&session,&o->old,&old);if(rc)return rc;
    Runs data;
    rc=allocate_runs(m,(bytes+BB-1u)/BB,&data);
    if(rc)goto done;
    rc=build_descriptors(m,o,&data);if(rc)goto done;
    for(uint32_t i=0;i<data.count && !rc;i++){
        const Extent *e=&data.e[i];
        for(uint32_t j=0;j<e->count;j++){
            if(cancelled(mh())){rc=IFS_FS_BUSY;break;}
            uint8_t p[BB]={0};
            rc=source(ctx,e->logical+j,p);if(rc)break;
            rc=write_new_block(m,e->physical+j,p);if(rc)break;
        }
    }
    if(!rc)rc=release_old(m,&o->old,old);
    if(!rc){
        put32(o->raw+8,bytes);stamp(o->raw,!o->old.bytes && !o->old.count);
        rc=stage_inode(m,o->old.ino,o->raw);
    }
done:
    runs_drop(&data);mem_drop(old);return rc;
}
typedef struct {
    const Inode *old;const Extent *ext;uint32_t cursor;
    uint32_t bytes,at,length;const uint8_t *data;
} FileSource;
static int file_source(void *ctx,uint32_t logical,uint8_t p[BB])
{
    FileSource *f=ctx;uint64_t at=(uint64_t)logical*BB;
    if(at<f->old->bytes){
        while(f->cursor<f->old->count &&
              logical>=f->ext[f->cursor].logical+f->ext[f->cursor].count)++f->cursor;
        if(f->cursor>=f->old->count)return IFS_FS_CORRUPT;
        const Extent *e=&f->ext[f->cursor];
        if(logical<e->logical)return IFS_FS_CORRUPT;
        int rc=read_bytes(mh(),(uint64_t)(e->physical+logical-e->logical)*BB,p,BB);
        if(rc)return rc;
        if(f->old->bytes-at<BB)memset(p+f->old->bytes-at,0,BB-(size_t)(f->old->bytes-at));
    }
    uint64_t begin=at>f->at?at:f->at,end=at+BB;
    uint64_t patch_end=(uint64_t)f->at+f->length;
    if(end>patch_end)end=patch_end;
    if(begin<end)memcpy(p+(size_t)(begin-at),f->data+(size_t)(begin-f->at),(size_t)(end-begin));
    if(f->bytes-at<BB)memset(p+f->bytes-at,0,BB-(size_t)(f->bytes-at));
    return IFS_FS_OK;
}
static int replace_file(Mutation *m,Object *o,uint32_t bytes,uint32_t at,const void *p,uint32_t n)
{
    Extent *ext=NULL;int rc=extents(&session,&o->old,&ext);if(rc)return rc;
    FileSource f={.old=&o->old,.ext=ext,.bytes=bytes,.at=at,.length=n,.data=p};
    rc=replace_stream(m,o,bytes,file_source,&f);
    mem_drop(ext);return rc;
}
/* Pure append and zero-extension retain existing owned payload. Only the
 * old partial EOF block needs redo protection. New extent tables replace
 * the previous native tables in the same transaction. This avoids
 * quadratic full-file COW during repeated streaming writes. */
static int append_file(Mutation *m,Object *o,uint32_t bytes,uint32_t at,
                       const void *data,uint32_t n)
{
    Extent *old=NULL;int rc=extents(&session,&o->old,&old);if(rc)return rc;
    Runs added={0},all={0};
    uint32_t before=(o->old.bytes+BB-1u)/BB,after=(bytes+BB-1u)/BB;
    rc=allocate_runs(m,after-before,&added);if(rc)goto done;
    all.capacity=o->old.count+added.count;
    if(all.capacity>32767u){rc=IFS_FS_NO_SPACE;goto done;}
    if(all.capacity){
        all.e=mem_new((size_t)all.capacity*sizeof(*all.e));
        if(!all.e){rc=IFS_FS_NO_MEMORY;goto done;}
    }
    for(uint32_t i=0;i<o->old.count;i++)all.e[all.count++]=old[i];
    for(uint32_t i=0;i<added.count;i++){
        Extent e=added.e[i];e.logical+=before;
        Extent *last=all.count?&all.e[all.count-1u]:NULL;
        if(last && last->physical+last->count==e.physical &&
           last->count+e.count<=255u &&
           (last->physical-session.g.first)/session.g.group==
           (e.physical-session.g.first)/session.g.group)last->count+=e.count;
        else all.e[all.count++]=e;
    }
    rc=build_descriptors(m,o,&all);if(rc)goto done;
    rc=release_tables(m,&o->old);if(rc)goto done;
    FileSource source={.old=&o->old,.ext=old,.bytes=bytes,.at=at,.length=n,.data=data};
    if(o->old.bytes%BB){
        uint8_t p[BB]={0};uint32_t logical=before-1u;
        rc=file_source(&source,logical,p);if(rc)goto done;
        const Extent *last=&old[o->old.count-1u];
        rc=tx_stage(&m->tx,(uint64_t)(last->physical+last->count-1u)*BB,p,BB);
        if(rc)goto done;
    }
    for(uint32_t i=0;i<added.count&&!rc;i++)
        for(uint32_t j=0;j<added.e[i].count;j++){
            if(cancelled(mh())){rc=IFS_FS_BUSY;break;}
            uint8_t p[BB]={0};
            rc=file_source(&source,before+added.e[i].logical+j,p);if(rc)break;
            rc=write_new_block(m,added.e[i].physical+j,p);if(rc)break;
        }
    if(!rc){
        put32(o->raw+8,bytes);stamp(o->raw,!o->old.bytes);
        rc=stage_inode(m,o->old.ino,o->raw);
    }
done:
    runs_drop(&all);runs_drop(&added);mem_drop(old);return rc;
}
static int names_append(Names *n,const Dirent *e)
{
    if(n->count==n->capacity){
        uint32_t next=n->capacity?n->capacity*2u:16u;
        if(next<n->capacity||next>UINT32_MAX/sizeof(*n->e))return IFS_FS_NO_MEMORY;
        Dirent *p=mem_new((size_t)next*sizeof(*p));if(!p)return IFS_FS_NO_MEMORY;
        if(n->e)memcpy(p,n->e,(size_t)n->count*sizeof(*p));
        mem_drop(n->e);n->e=p;n->capacity=next;
    }
    n->e[n->count++]=*e;return IFS_FS_OK;
}
static int names_visit(void *ctx,const Dirent *e){return names_append(ctx,e);}
static int load_names(const Inode *dir,Names *n)
{
    *n=(Names){0};int rc=directory_walk(&session,dir,names_visit,n);
    if(rc)names_drop(n);
    return rc;
}
static int names_index(const Names *n,const char *name,uint32_t len,uint32_t *index)
{
    for(uint32_t i=0;i<n->count;i++)if(n->e[i].len==len&&!memcmp(n->e[i].name,name,len)){
        *index=i;return IFS_FS_OK;
    }
    return IFS_FS_NOT_FOUND;
}
static void names_remove(Names *n,uint32_t index)
{
    memmove(n->e+index,n->e+index+1u,(size_t)(n->count-index-1u)*sizeof(*n->e));--n->count;
}
static int dir_pack(const Names *n,uint8_t **data,uint32_t *bytes)
{
    *data=NULL;*bytes=0;
    if(!n->count||n->count>INT32_MAX/BB)return IFS_FS_NO_SPACE;
    uint8_t *p=mem_new((size_t)n->count*BB);if(!p)return IFS_FS_NO_MEMORY;
    uint32_t blocks=1,off=BB,slots=0;uint8_t *b=p;
    memset(b,0,BB);put16(b,0xbeef);
    for(uint32_t i=0;i<n->count;i++){
        const Dirent *e=&n->e[i];
        uint32_t need=(5u+e->len+1u)&~1u;
        if(off<need+4u+slots+1u||slots==255u){
            b=p+(size_t)blocks++*BB;memset(b,0,BB);put16(b,0xbeef);
            off=BB;slots=0;
        }
        off-=need;
        if(off<4u+slots+1u){mem_drop(p);return IFS_FS_INVALID;}
        b[4u+slots++]=(uint8_t)(off/2u);b[2]=(uint8_t)(off/2u);b[3]=(uint8_t)slots;
        put32(b+off,e->ino);b[off+4]=(uint8_t)e->len;memcpy(b+off+5,e->name,e->len);
    }
    *data=p;*bytes=blocks*BB;return IFS_FS_OK;
}
static int memory_source(void *ctx,uint32_t logical,uint8_t p[BB])
{memcpy(p,(const uint8_t*)ctx+(size_t)logical*BB,BB);return IFS_FS_OK;}
static int replace_directory(Mutation *m,Object *o,const Names *n)
{
    uint8_t *p=NULL;uint32_t bytes=0;int rc=dir_pack(n,&p,&bytes);
    if(!rc)rc=replace_stream(m,o,bytes,memory_source,p);
    mem_drop(p);return rc;
}
static void put_hex32(char *p,uint32_t n)
{
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<8;i++)p[i]=hex[(n>>(28u-i*4u))&15u];
}
static Dirent reserve_name(const Object *target,uint32_t ino)
{
    Dirent e={.ino=ino,.len=17,.name="r"};
    put_hex32(e.name+1,target->old.ino);put_hex32(e.name+9,be32(target->raw+24));
    return e;
}
static int reservation_directory(Object *dir,Names *names)
{
    if(!session.reserve_dir)return IFS_FS_UNSUPPORTED;
    int rc=load_object(session.reserve_dir,dir);
    if(!rc)rc=load_names(&dir->old,names);
    return rc;
}
static int reservation_owner(const Reservation *r,Object *o)
{return r?load_object(r->inode,o):IFS_FS_NOT_FOUND;}
/* A native ordinary inode owns each zero-filled reserve block. */
static int reservation_create(Mutation *m,const Object *target,const Runs *runs,uint32_t count)
{
    const Reservation *previous=reservation_for(&session,target->old.ino);
    Object owner,dir;Names names={0};Extent *old=NULL;
    int rc=previous?reservation_owner(previous,&owner):new_inode(m,IFS_FS_ENTRY_FILE,&owner);
    if(rc)goto done;
    put16(owner.raw,0100600u);
    rc=extents(&session,&owner.old,&old);if(rc)goto done;
    rc=build_descriptors(m,&owner,runs);if(rc)goto done;
    for(uint32_t i=0;i<runs->count&&!rc;i++)
        for(uint32_t k=0;k<runs->e[i].count;k++){
            uint8_t zero[BB]={0};
            if(cancelled(mh())){rc=IFS_FS_BUSY;break;}
            rc=write_new_block(m,runs->e[i].physical+k,zero);if(rc)break;
        }
    if(!rc)rc=release_old(m,&owner.old,old);
    if(!rc){
        put32(owner.raw+8,count*BB);stamp(owner.raw,!previous);
        rc=stage_inode(m,owner.old.ino,owner.raw);
    }
    if(!rc&&!previous){
        rc=reservation_directory(&dir,&names);
        Dirent e=reserve_name(target,owner.old.ino);
        if(!rc)rc=names_append(&names,&e);
        if(!rc)rc=replace_directory(m,&dir,&names);
    }
done:
    mem_drop(old);names_drop(&names);return rc;
}
static int reservation_delete(Mutation *m,uint32_t target,int release_data)
{
    const Reservation *r=reservation_for(&session,target);
    if(!r)return IFS_FS_OK;
    Object owner,dir;Names names={0};Extent *old=NULL;
    int rc=reservation_owner(r,&owner);if(rc)goto done;
    if(release_data){
        rc=extents(&session,&owner.old,&old);
        if(!rc)rc=release_old(m,&owner.old,old);
    }else rc=release_tables(m,&owner.old);
    if(rc)goto done;
    rc=reservation_directory(&dir,&names);if(rc)goto done;
    uint32_t i=0;while(i<names.count&&names.e[i].ino!=r->inode)++i;
    if(i==names.count){rc=IFS_FS_CORRUPT;goto done;}
    names_remove(&names,i);
    uint32_t gen=be32(owner.raw+24);
    memset(owner.raw,0,DINODE);put32(owner.raw+24,gen);
    ++m->free_inodes;
    rc=stage_inode(m,owner.old.ino,owner.raw);
    if(!rc)rc=replace_directory(m,&dir,&names);
done:
    mem_drop(old);names_drop(&names);return rc;
}
/* A reserve is a native allocated owner, not free space. Transfer its
 * blocks to the target in the same transaction as the new inode sizes.
 * Keep all existing user payload sectors journal-protected. A whole
 * physical page entirely inside the reserve may receive new payload
 * before PREPARED: it has no prior user-visible file contents. */
static int write_growth_block(Mutation *m,const Reservation *r,uint32_t block,const uint8_t p[BB])
{
    uint32_t page=block/(JUNIT/BB),first=page*(JUNIT/BB);
    int private_page=first>=r->start && first+JUNIT/BB<=r->start+r->count;
    for(uint32_t i=0;i<m->tx.count;i++)if(m->tx.page[i]==page)private_page=0;
    if(!private_page)return tx_stage(&m->tx,(uint64_t)block*BB,p,BB);
    uint8_t full[JUNIT];
    int rc=jread(mh(),(uint64_t)page*JUNIT,full);if(rc)return rc;
    memcpy(full+(block-first)*BB,p,BB);
    return jwrite(mh(),(uint64_t)page*JUNIT,full);
}
static int shrink_reservation(Mutation *m,const Reservation *r,uint32_t used)
{
    if(!used)return IFS_FS_OK;
    if(used>r->count)return IFS_FS_CORRUPT;
    if(used==r->count)return reservation_delete(m,r->target,0);
    Object owner;int rc=reservation_owner(r,&owner);if(rc)return rc;
    Runs runs={0};
    rc=exact_runs(m,r->start+used,r->count-used,0,&runs);
    if(!rc)rc=build_descriptors(m,&owner,&runs);
    if(!rc)rc=release_tables(m,&owner.old);
    if(!rc){
        put32(owner.raw+8,(r->count-used)*BB);stamp(owner.raw,0);
        rc=stage_inode(m,owner.old.ino,owner.raw);
    }
    runs_drop(&runs);return rc;
}
static int can_use_reservation(const Object *o,const Reservation *r,uint32_t bytes,
                               uint32_t at,uint32_t n)
{
    if(!r||bytes<o->old.bytes)return 0;
    uint32_t old_blocks=(o->old.bytes+BB-1u)/BB,new_blocks=(bytes+BB-1u)/BB;
    if(new_blocks-old_blocks>r->count)return 0;
    /* Existing user data must be in the bounded redo journal. Large
     * overwrites use the ordinary atomic COW path rather than splitting
     * an API write into partly committed transactions. */
    if(n&&at<o->old.bytes){
        uint32_t affected=o->old.bytes-at;if(affected>n)affected=n;
        if(affected>(JMAX-12u)*JUNIT)return 0;
    }
    return 1;
}
static int use_reservation(Mutation *m,Object *o,const Reservation *r,
                           uint32_t bytes,uint32_t at,const void *data,uint32_t n)
{
    uint32_t old_blocks=(o->old.bytes+BB-1u)/BB,new_blocks=(bytes+BB-1u)/BB;
    uint32_t used=new_blocks-old_blocks,start=0;
    Extent *old=NULL;int rc=extents(&session,&o->old,&old);if(rc)return rc;
    Runs target={0};
    if(!physical_run(&o->old,old,&start)||start+old_blocks!=r->start||used>r->count){
        rc=IFS_FS_CORRUPT;goto done;
    }
    if(used){
        rc=exact_runs(m,start,new_blocks,0,&target);if(rc)goto done;
        rc=build_descriptors(m,o,&target);if(rc)goto done;
        rc=release_tables(m,&o->old);if(rc)goto done;
    }
    uint32_t first=n?at/BB:o->old.bytes/BB;
    uint32_t end=n?(at+n+BB-1u)/BB:0u;
    if(bytes>o->old.bytes){
        uint32_t old_end=o->old.bytes/BB;
        if(first>old_end)first=old_end;
        if(end<new_blocks)end=new_blocks;
    }
    FileSource source={.old=&o->old,.ext=old,.bytes=bytes,.at=at,.length=n,.data=data};
    for(uint32_t logical=first;logical<end;logical++){
        if(cancelled(mh())){rc=IFS_FS_BUSY;goto done;}
        uint8_t p[BB]={0};rc=file_source(&source,logical,p);if(rc)goto done;
        rc=write_growth_block(m,r,start+logical,p);if(rc)goto done;
    }
    put32(o->raw+8,bytes);stamp(o->raw,0);
    rc=stage_inode(m,o->old.ino,o->raw);
    if(!rc)rc=shrink_reservation(m,r,used);
done:
    runs_drop(&target);mem_drop(old);return rc;
}
static int rewrite_with_reservation(Mutation *m,Object *o,uint32_t bytes,
                                    uint32_t at,const void *p,uint32_t n)
{
    const Reservation *r=reservation_for(&session,o->old.ino);
    if(can_use_reservation(o,r,bytes,at,n))
        return use_reservation(m,o,r,bytes,at,p,n);
    int rc=reservation_delete(m,o->old.ino,1);
    if(rc)return rc;
    if(!r && bytes>o->old.bytes && (!n || at>=o->old.bytes))
        return append_file(m,o,bytes,at,p,n);
    return replace_file(m,o,bytes,at,p,n);
}
static int resolve_parent(const char *path,Object *parent,Dirent *leaf)
{
    if(!path||path[0]!='/')return IFS_FS_INVALID;
    size_t len=0;while(len<=65536u && path[len])++len;
    if(len<2u||len>65536u)return IFS_FS_INVALID;
    for(size_t i=1;i<len;i++)if(path[i]=='/'&&path[i-1u]=='/')return IFS_FS_INVALID;
    const char *slash=strrchr(path,'/');size_t n=(size_t)(path+len-slash-1);
    if(!n||n>MAX_NAME||(n==1u&&slash[1]=='.')||
       (n==2u&&slash[1]=='.'&&slash[2]=='.'))return IFS_FS_INVALID;
    *leaf=(Dirent){.len=(uint32_t)n};memcpy(leaf->name,slash+1,n);
    size_t plen=(size_t)(slash-path);if(!plen)plen=1u;
    char *p=mem_new(plen+1u);if(!p)return IFS_FS_NO_MEMORY;
    memcpy(p,path,plen);p[plen]=0;
    Inode ino;Dirent ent;int rc=resolve(&session,p,&ino,&ent);mem_drop(p);
    if(!rc && ino.type!=IFS_FS_ENTRY_DIRECTORY)rc=IFS_FS_INVALID;
    if(!rc && ino.ino==ROOT && session.journal_owned && internal_name(leaf->name,leaf->len))
        rc=IFS_FS_ACCESS;
    return rc?rc:load_object(ino.ino,parent);
}
static int change_links(Object *o,int delta)
{
    uint32_t n=be16(o->raw+2);
    if((delta<0&&n<2u)||(delta>0&&n==UINT16_MAX))return IFS_FS_NO_SPACE;
    put16(o->raw+2,delta>0?n+1u:n-1u);return IFS_FS_OK;
}
static int create_impl(void *v,const char *path,uint32_t type)
{
    if(type!=IFS_FS_ENTRY_FILE&&type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_UNSUPPORTED;
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Object parent,child;Dirent leaf;Names entries={0},dots={0};uint32_t index=0;
    rc=resolve_parent(path,&parent,&leaf);if(rc)goto done;
    rc=load_names(&parent.old,&entries);if(rc)goto done;
    if(names_index(&entries,leaf.name,leaf.len,&index)==IFS_FS_OK){rc=IFS_FS_EXISTS;goto done;}
    rc=new_inode(&m,type,&child);if(rc)goto done;
    leaf.ino=child.old.ino;rc=names_append(&entries,&leaf);if(rc)goto done;
    if(type==IFS_FS_ENTRY_DIRECTORY){
        Dirent dot={.ino=child.old.ino,.len=1,.name="."};
        Dirent up={.ino=parent.old.ino,.len=2,.name=".."};
        rc=names_append(&dots,&dot);if(!rc)rc=names_append(&dots,&up);
        if(!rc)rc=change_links(&parent,1);
        if(!rc)rc=replace_directory(&m,&child,&dots);
    }else {stamp(child.raw,1);rc=stage_inode(&m,child.old.ino,child.raw);}
    if(!rc)rc=replace_directory(&m,&parent,&entries);
    if(!rc)rc=mutation_commit(&m);
done:
    names_drop(&entries);names_drop(&dots);mutation_end(&m);return rc;
}
static int write_impl(void *v,const char *path,uint64_t at,const void *p,uint32_t n)
{
    if((!p&&n)||at>INT32_MAX||n>INT32_MAX-at)return IFS_FS_INVALID;
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Inode ino;Dirent ent;Object o;
    rc=resolve(&session,path,&ino,&ent);
    if(!rc && ino.type!=IFS_FS_ENTRY_FILE)rc=IFS_FS_UNSUPPORTED;
    if(!rc && n){
        rc=load_object(ino.ino,&o);
        uint32_t bytes=ino.bytes>at+n?ino.bytes:(uint32_t)(at+n);
        if(!rc)rc=rewrite_with_reservation(&m,&o,bytes,(uint32_t)at,p,n);
        if(!rc)rc=mutation_commit(&m);
    }
    mutation_end(&m);return rc;
}
static int truncate_impl(void *v,const char *path,uint64_t bytes)
{
    if(bytes>INT32_MAX)return IFS_FS_INVALID;
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Inode ino;Dirent ent;Object o;
    rc=resolve(&session,path,&ino,&ent);
    if(!rc && ino.type!=IFS_FS_ENTRY_FILE)rc=IFS_FS_UNSUPPORTED;
    if(!rc && bytes!=ino.bytes){
        rc=load_object(ino.ino,&o);
        if(!rc)rc=rewrite_with_reservation(&m,&o,(uint32_t)bytes,0,NULL,0);
        if(!rc)rc=mutation_commit(&m);
    }
    mutation_end(&m);return rc;
}
static int remove_impl(void *v,const char *path)
{
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Object parent,child;Dirent leaf;Names entries={0},children={0};Extent *old=NULL;
    uint32_t index=0;rc=resolve_parent(path,&parent,&leaf);if(rc)goto done;
    rc=load_names(&parent.old,&entries);if(rc)goto done;
    rc=names_index(&entries,leaf.name,leaf.len,&index);if(rc)goto done;
    rc=load_object(entries.e[index].ino,&child);if(rc)goto done;
    if(child.old.type==IFS_FS_ENTRY_DIRECTORY){
        rc=load_names(&child.old,&children);if(rc)goto done;
        if(children.count!=2u||be16(child.raw+2)!=2u){rc=IFS_FS_BUSY;goto done;}
        rc=change_links(&parent,-1);if(rc)goto done;
    }else if(child.old.type!=IFS_FS_ENTRY_FILE){rc=IFS_FS_UNSUPPORTED;goto done;}
    names_remove(&entries,index);
    if(child.old.type==IFS_FS_ENTRY_FILE&&be16(child.raw+2)>1u){
        rc=change_links(&child,-1);
        if(!rc)rc=stage_inode(&m,child.old.ino,child.raw);
    }else{
        rc=reservation_delete(&m,child.old.ino,1);
        if(!rc)rc=extents(&session,&child.old,&old);
        if(!rc)rc=release_old(&m,&child.old,old);
        if(!rc){
            uint32_t gen=be32(child.raw+24);
            memset(child.raw,0,DINODE);put32(child.raw+24,gen);
            ++m.free_inodes;rc=stage_inode(&m,child.old.ino,child.raw);
        }
    }
    if(!rc)rc=replace_directory(&m,&parent,&entries);
    if(!rc)rc=mutation_commit(&m);
done:
    mem_drop(old);names_drop(&entries);names_drop(&children);mutation_end(&m);return rc;
}
static int rename_impl(void *v,const char *old_path,const char *new_path)
{
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Object from,to,child;Dirent old_name,new_name;Names a={0},b={0},dots={0};
    uint32_t oi=0,ni=0;
    rc=resolve_parent(old_path,&from,&old_name);if(rc)goto done;
    rc=resolve_parent(new_path,&to,&new_name);if(rc)goto done;
    rc=load_names(&from.old,&a);if(rc)goto done;
    rc=names_index(&a,old_name.name,old_name.len,&oi);if(rc)goto done;
    rc=load_object(a.e[oi].ino,&child);if(rc)goto done;
    new_name.ino=child.old.ino;
    if(from.old.ino==to.old.ino){
        if(names_index(&a,new_name.name,new_name.len,&ni)==IFS_FS_OK){
            rc=ni==oi?IFS_FS_OK:IFS_FS_EXISTS;goto done;
        }
        a.e[oi]=new_name;rc=replace_directory(&m,&from,&a);
    }else{
        rc=load_names(&to.old,&b);if(rc)goto done;
        if(names_index(&b,new_name.name,new_name.len,&ni)==IFS_FS_OK){rc=IFS_FS_EXISTS;goto done;}
        if(child.old.type==IFS_FS_ENTRY_DIRECTORY){
            /* All directory paths have unique parent chains after CHECK;
             * canonical absolute path prefix detects descendant moves. */
            size_t n=strlen(old_path);
            if(strlen(new_path)>n && !memcmp(old_path,new_path,n)&&new_path[n]=='/'){
                rc=IFS_FS_INVALID;goto done;
            }
            rc=load_names(&child.old,&dots);if(rc)goto done;
            rc=names_index(&dots,"..",2,&ni);if(rc)goto done;
            dots.e[ni].ino=to.old.ino;
            rc=change_links(&from,-1);if(!rc)rc=change_links(&to,1);
            if(!rc)rc=replace_directory(&m,&child,&dots);
            if(rc)goto done;
        }
        names_remove(&a,oi);rc=names_append(&b,&new_name);
        if(!rc)rc=replace_directory(&m,&from,&a);
        if(!rc)rc=replace_directory(&m,&to,&b);
    }
    if(!rc)rc=mutation_commit(&m);
done:
    names_drop(&a);names_drop(&b);names_drop(&dots);mutation_end(&m);return rc;
}
