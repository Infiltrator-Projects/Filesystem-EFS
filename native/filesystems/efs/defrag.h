/* SPDX-License-Identifier: GPL-3.0-or-later
 * EFS native physical relocation. PLAN is a read-only simulation over
 * the native allocation bitmap, EXECUTE commits one COW file move at a
 * time, and VERIFY rereads the actual extents and all payload bytes.
 * No caller supplies physical locations or interprets extent metadata.
 */
typedef struct {
    uint32_t ino,bytes,start,crc;
    int move;
} PlanFile;
typedef struct {
    uintptr_t token;
    PlanFile *files;
    uint32_t count,capacity,next;
    uint64_t moved;
    int complete;
} DefragPlan;
static DefragPlan *plan_state;
static uintptr_t plan_generation;
static uint32_t crc_feed(uint32_t c,const uint8_t *p,size_t n)
{
    while(n--){
        c^=*p++;
        for(unsigned i=0;i<8;i++)c=(c>>1)^((0u-(c&1u))&UINT32_C(0xedb88320));
    }
    return c;
}
static int inode_crc(const Inode *ino,const Extent *e,uint32_t *crc)
{
    uint32_t c=UINT32_MAX,left=ino->bytes;
    for(uint32_t i=0;i<ino->count;i++){
        uint32_t n=e[i].count*BB;
        if(n>left)n=left;
        uint64_t at=(uint64_t)e[i].physical*BB;
        while(n){
            if(cancelled(mh()))return IFS_FS_BUSY;
            uint8_t p[JUNIT];uint32_t take=n>sizeof p?sizeof p:n;
            int rc=read_bytes(mh(),at,p,take);if(rc)return rc;
            c=crc_feed(c,p,take);n-=take;at+=take;left-=take;
        }
    }
    if(left)return IFS_FS_CORRUPT;
    *crc=~c;return IFS_FS_OK;
}
static int contiguous(const Inode *ino,const Extent *e,uint32_t *start)
{
    *start=0;
    if(!ino->count)return !ino->bytes;
    *start=e[0].physical;
    for(uint32_t i=1;i<ino->count;i++)
        if(e[i-1u].physical+e[i-1u].count!=e[i].physical)return 0;
    return 1;
}
static int plan_append(DefragPlan *p,uint32_t ino)
{
    if(p->count==p->capacity){
        uint32_t next=p->capacity?p->capacity*2u:32u;
        if(next<p->capacity||next>UINT32_MAX/sizeof(*p->files))return IFS_FS_NO_MEMORY;
        PlanFile *f=mem_new((size_t)next*sizeof(*f));if(!f)return IFS_FS_NO_MEMORY;
        if(p->count)memcpy(f,p->files,(size_t)p->count*sizeof(*f));
        mem_drop(p->files);p->files=f;p->capacity=next;
    }
    p->files[p->count++]=(PlanFile){.ino=ino};return IFS_FS_OK;
}
static int plan_collect(DefragPlan *p,uint8_t *seen,uint32_t ino,unsigned depth)
{
    if(depth>256u)return IFS_FS_UNSUPPORTED;
    if(cancelled(mh()))return IFS_FS_BUSY;
    uint8_t mask=(uint8_t)(1u<<(ino%8u));
    if(seen[ino/8u]&mask)return IFS_FS_OK; /* hard-linked file only once */
    seen[ino/8u]|=mask;
    uint8_t raw[DINODE];int rc=inode_raw(&session,ino,raw);if(rc)return rc;
    uint32_t mode=be16(raw)&0170000u;
    if(mode==REG)return plan_append(p,ino);
    if(mode!=DIR)return IFS_FS_OK;
    Inode dir;rc=inode_decode(ino,raw,&dir);if(rc)return rc;
    Names n={0};rc=load_names(&dir,&n);
    if(!rc)for(uint32_t i=0;i<n.count;i++)if(!dot_kind(&n.e[i])){
        rc=plan_collect(p,seen,n.e[i].ino,depth+1u);if(rc)break;
    }
    names_drop(&n);return rc;
}
static int choose_run(const uint8_t *bitmap,uint32_t blocks,uint32_t *first)
{
    *first=0;
    for(uint32_t cg=0;cg<session.g.groups;cg++){
        uint32_t start=session.g.first+cg*session.g.group+session.g.inodes;
        uint32_t end=session.g.first+(cg+1u)*session.g.group,run=0;
        for(uint32_t b=start;b<end;b++){
            if((b&4095u)==0 && cancelled(mh()))return IFS_FS_BUSY;
            run=free_bit(bitmap,b)?run+1u:0u;
            if(run==blocks){*first=b+1u-blocks;return IFS_FS_OK;}
        }
    }
    return IFS_FS_NO_SPACE;
}
static void sim_alloc(uint8_t *bitmap,uint32_t b)
{bitmap[b/8u]&=(uint8_t)~(1u<<(b%8u));}
/* Simulate exactly the table allocator used by build_descriptors: first
 * free native data blocks, coalesced up to 255 blocks within a group.
 * Old inode data and tables remain allocated until the destination is
 * fully allocated, so no move can depend on overwriting its own source. */
static int simulate_tables(uint8_t *bitmap,uint32_t blocks)
{
    uint32_t need=(blocks+254u)/255u;
    if(need<=DIRECT)return IFS_FS_OK;
    need=(need+63u)/64u;
    uint32_t count=0,last=0,run=0;
    for(uint32_t cg=0;cg<session.g.groups && need;cg++){
        uint32_t start=session.g.first+cg*session.g.group+session.g.inodes;
        uint32_t end=session.g.first+(cg+1u)*session.g.group;
        for(uint32_t b=start;b<end && need;b++){
            if(!free_bit(bitmap,b))continue;
            if(run && last+1u==b && run<255u)++run;
            else {if(++count>DIRECT)return IFS_FS_NO_SPACE;run=1;}
            last=b;sim_alloc(bitmap,b);--need;
        }
        run=0;
    }
    return need?IFS_FS_NO_SPACE:IFS_FS_OK;
}
static int simulate_file(uint8_t *bitmap,PlanFile *f)
{
    Inode ino;int rc=inode_load(&session,f->ino,&ino);if(rc)return rc;
    if(ino.type!=IFS_FS_ENTRY_FILE)return IFS_FS_CORRUPT;
    f->bytes=ino.bytes;
    Extent *old=NULL;rc=extents(&session,&ino,&old);if(rc)return rc;
    rc=inode_crc(&ino,old,&f->crc);if(rc)goto done;
    uint32_t blocks=(ino.bytes+BB-1u)/BB,start=0,dest=0;
    int whole=contiguous(&ino,old,&start);
    f->start=start;
    if(!blocks)goto done;
    rc=choose_run(bitmap,blocks,&dest);
    if(rc==IFS_FS_NO_SPACE && whole){rc=IFS_FS_OK;goto done;}
    if(rc)goto done;
    if(whole && dest>=start)goto done;
    f->start=dest;f->move=1;
    for(uint32_t i=0;i<blocks;i++)sim_alloc(bitmap,dest+i);
    rc=simulate_tables(bitmap,blocks);if(rc)goto done;
    for(uint32_t i=0;i<ino.count;i++)
        for(uint32_t k=0;k<old[i].count;k++)set_free(bitmap,old[i].physical+k);
    if(ino.count>DIRECT){
        for(uint32_t i=0;i<be24(ino.descriptors+5);i++){
            Extent table;rc=decode_extent(&session,ino.descriptors+8u*i,&table);if(rc)goto done;
            for(uint32_t k=0;k<table.count;k++)set_free(bitmap,table.physical+k);
        }
    }
done:
    mem_drop(old);return rc;
}
static void plan_drop(DefragPlan *p)
{if(p){mem_drop(p->files);mem_drop(p);}}
static int plan_impl(void *v,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!live(v))return IFS_FS_INVALID;
    if(!session.writable)return IFS_FS_ACCESS;
    if(active_plan)return IFS_FS_BUSY;
    /* Policy 10 requires an actual persistent native reservation. Do
     * not advertise ordinary contiguous free space as a reserved extent. */
    if(policy)return IFS_FS_UNSUPPORTED;
    if(plan_generation==UINTPTR_MAX)return IFS_FS_NO_SPACE;
    int rc=check_native(&session);if(rc)return rc;
    Inode root;Dirent ent;rc=resolve(&session,path,&root,&ent);if(rc)return rc;
    if(root.type!=IFS_FS_ENTRY_FILE && root.type!=IFS_FS_ENTRY_DIRECTORY)
        return IFS_FS_UNSUPPORTED;
    DefragPlan *p=mem_new(sizeof *p);
    uint8_t *bitmap=mem_new(session.g.bitmap_bytes);
    uint32_t total=session.g.inodes*session.g.groups*4u;
    uint8_t *seen=mem_new((total+7u)/8u);
    if(!p||!bitmap||!seen){
        mem_drop(p);mem_drop(bitmap);mem_drop(seen);return IFS_FS_NO_MEMORY;
    }
    *p=(DefragPlan){0};memset(seen,0,(total+7u)/8u);
    memcpy(bitmap,session.bitmap,session.g.bitmap_bytes);
    rc=plan_collect(p,seen,root.ino,0);
    if(!rc)for(uint32_t i=0;i<p->count;i++){rc=simulate_file(bitmap,&p->files[i]);if(rc)break;}
    mem_drop(bitmap);mem_drop(seen);
    if(rc){plan_drop(p);return rc;}
    p->token=++plan_generation;active_plan=p->token;plan_state=p;
    *out=(void*)p->token;return IFS_FS_OK;
}
static int verify_file(const PlanFile *f)
{
    Inode ino;int rc=inode_load(&session,f->ino,&ino);if(rc)return rc;
    if(ino.type!=IFS_FS_ENTRY_FILE||ino.bytes!=f->bytes)return IFS_FS_CORRUPT;
    Extent *ext=NULL;rc=extents(&session,&ino,&ext);if(rc)return rc;
    uint32_t start=0,crc=0;
    if(!contiguous(&ino,ext,&start)||start!=f->start)rc=IFS_FS_CORRUPT;
    if(!rc)rc=inode_crc(&ino,ext,&crc);
    if(!rc && crc!=f->crc)rc=IFS_FS_CORRUPT;
    mem_drop(ext);return rc;
}
static int relocate_file(void *v,const PlanFile *f)
{
    Object o;int rc=load_object(f->ino,&o);if(rc)return rc;
    if(o.old.type!=IFS_FS_ENTRY_FILE||o.old.bytes!=f->bytes)return IFS_FS_CORRUPT;
    Extent *old=NULL;rc=extents(&session,&o.old,&old);if(rc)return rc;
    uint32_t crc=0;rc=inode_crc(&o.old,old,&crc);
    if(!rc && crc!=f->crc)rc=IFS_FS_CORRUPT;
    if(rc){mem_drop(old);return rc;}
    Mutation m;rc=mutation_begin(&m,v);if(rc){mem_drop(old);return rc;}
    Runs runs={0};
    uint32_t blocks=(f->bytes+BB-1u)/BB;
    runs.capacity=(blocks+254u)/255u;
    runs.e=mem_new((size_t)runs.capacity*sizeof(*runs.e));
    if(!runs.e){rc=IFS_FS_NO_MEMORY;goto done;}
    for(uint32_t i=0;i<blocks;i++){
        uint32_t b=f->start+i;
        if(!data_block(&session.g,b)||!free_bit(session.bitmap,b)){
            rc=IFS_FS_CORRUPT;goto done;
        }
        rc=mark_allocated(&m,b);if(rc)goto done;
        if(!i || runs.e[runs.count-1u].count==255u)
            runs.e[runs.count++]=(Extent){.physical=b,.logical=i,.count=1};
        else ++runs.e[runs.count-1u].count;
    }
    rc=build_descriptors(&m,&o,&runs);if(rc)goto done;
    FileSource source={.old=&o.old,.ext=old,.bytes=f->bytes};
    for(uint32_t i=0;i<blocks;i++){
        if(cancelled(mh())){rc=IFS_FS_BUSY;goto done;}
        uint8_t p[BB]={0};rc=file_source(&source,i,p);if(rc)goto done;
        rc=write_new_block(&m,f->start+i,p);if(rc)goto done;
    }
    rc=release_old(&m,&o.old,old);if(rc)goto done;
    /* Logical file data and timestamps do not change during relocation. */
    rc=stage_inode(&m,o.old.ino,o.raw);
    if(!rc)rc=mutation_commit(&m);
done:
    runs_drop(&runs);mutation_end(&m);mem_drop(old);return rc;
}
static int plan_matches(void *v,void *p)
{return live(v)&&p&&active_plan==(uintptr_t)p&&plan_state&&plan_state->token==(uintptr_t)p;}
static int execute_impl(void *v,void *p,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0;
    if(!plan_matches(v,p))return IFS_FS_INVALID;
    DefragPlan *q=plan_state;
    if(q->complete){*moved=q->moved;return IFS_FS_OK;}
    int rc=IFS_FS_OK;
    plan_executing=1;
    while(q->next<q->count){
        if(cancelled(mh())){rc=IFS_FS_BUSY;break;}
        PlanFile *f=&q->files[q->next];
        if(f->move){
            rc=relocate_file(v,f);if(rc)break;
            q->moved+=f->bytes;
        }
        rc=verify_file(f);if(rc){session.fault=1;break;}
        ++q->next;
    }
    plan_executing=0;
    if(!rc)q->complete=1;
    *moved=q->moved;return rc;
}
static int verify_impl(void *v,void *p)
{
    if(!plan_matches(v,p))return IFS_FS_INVALID;
    if(!plan_state->complete)return IFS_FS_BUSY;
    for(uint32_t i=0;i<plan_state->count;i++){
        int rc=verify_file(&plan_state->files[i]);if(rc)return rc;
    }
    return check_native(&session);
}
static void release_impl(void *v,void *p)
{
    if(!v||(uintptr_t)v!=session.token||!p||(uintptr_t)p!=active_plan)return;
    plan_drop(plan_state);plan_state=NULL;active_plan=0;
}
