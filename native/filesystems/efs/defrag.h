/* SPDX-License-Identifier: GPL-3.0-or-later
 * EFS native physical relocation. PLAN is a read-only simulation over
 * the native allocation bitmap, EXECUTE commits one COW file move at a
 * time, and VERIFY rereads the actual extents and all payload bytes.
 * No caller supplies physical locations or interprets extent metadata.
 */
typedef struct {
    uint32_t ino,bytes,start,crc,growth;
    int move,reserve_only;
} PlanFile;
typedef struct {
    uintptr_t token;
    PlanFile *files;
    uint8_t *avoid;
    uint32_t count,capacity,next,policy;
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
    return physical_run(ino,e,start);
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
    if(ino==session.reserve_dir)return IFS_FS_OK;
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
    /* Ordinary compaction must not destroy an existing growth reserve. */
    if(reservation_for(&session,ino.ino))goto done;
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
/* Preflight protects every planned target from metadata allocation.
 * Conservatively keep all old allocations occupied in the simulation. */
static int free_run_at(const uint8_t *bitmap,uint32_t start,uint32_t count)
{
    if(!count||!data_block(&session.g,start)||count>session.g.size-start||
       count>session.g.group-(start-session.g.first)%session.g.group)return 0;
    for(uint32_t i=0;i<count;i++)if(!free_bit(bitmap,start+i))return 0;
    return 1;
}
static void reserve_plan_run(uint8_t *bitmap,uint8_t *avoid,uint32_t start,uint32_t n)
{
    for(uint32_t i=0;i<n;i++){sim_alloc(bitmap,start+i);set_free(avoid,start+i);}
}
static int simulate_growth(uint8_t *bitmap,uint8_t *avoid,PlanFile *f,
                           uint32_t *new_owners,uint64_t *extra)
{
    Inode ino;int rc=inode_load(&session,f->ino,&ino);if(rc)return rc;
    if(ino.type!=IFS_FS_ENTRY_FILE)return IFS_FS_CORRUPT;
    f->bytes=ino.bytes;Extent *old=NULL;rc=extents(&session,&ino,&old);if(rc)return rc;
    rc=inode_crc(&ino,old,&f->crc);if(rc)goto done;
    uint32_t blocks=(ino.bytes+BB-1u)/BB,start=0,dest=0;
    int whole=contiguous(&ino,old,&start);f->start=start;
    if(!blocks)goto done;
    f->growth=(blocks+9u)/10u;
    const Reservation *r=reservation_for(&session,ino.ino);
    if(r&&whole&&r->count==f->growth)goto done;
    if(!r && whole && free_run_at(bitmap,start+blocks,f->growth)){
        f->reserve_only=1;reserve_plan_run(bitmap,avoid,start+blocks,f->growth);
    }else{
        rc=choose_run(bitmap,blocks+f->growth,&dest);if(rc)goto done;
        f->start=dest;f->move=1;
        reserve_plan_run(bitmap,avoid,dest,blocks+f->growth);
        uint32_t n=(blocks+254u)/255u;
        if(n>DIRECT)*extra+=(n+63u)/64u;
    }
    if(!r)++*new_owners;
    uint32_t n=(f->growth+254u)/255u;
    if(n>DIRECT)*extra+=(n+63u)/64u;
done:
    mem_drop(old);return rc;
}
static int growth_preflight(uint8_t *bitmap,DefragPlan *p)
{
    if(!session.reserve_dir)return IFS_FS_UNSUPPORTED;
    uint32_t owners=0;uint64_t extra=0;int rc=IFS_FS_OK;
    for(uint32_t i=0;i<p->count;i++){
        rc=simulate_growth(bitmap,p->avoid,&p->files[i],&owners,&extra);if(rc)return rc;
    }
    if(owners>session.g.free_inodes)return IFS_FS_NO_SPACE;
    if(owners){
        Inode dir;rc=inode_load(&session,session.reserve_dir,&dir);if(rc)return rc;
        /* One whole block per new name is a conservative upper bound. */
        uint64_t dir_blocks=(uint64_t)dir.bytes/BB+owners;
        extra+=dir_blocks+(dir_blocks+63u)/64u;
    }
    uint64_t free=0;
    for(uint32_t cg=0;cg<session.g.groups;cg++){
        uint32_t start=session.g.first+cg*session.g.group+session.g.inodes;
        uint32_t end=session.g.first+(cg+1u)*session.g.group;
        if(cancelled(mh()))return IFS_FS_BUSY;
        for(uint32_t b=start;b<end;b++)free+=(unsigned)free_bit(bitmap,b);
    }
    return free<extra?IFS_FS_NO_SPACE:IFS_FS_OK;
}
static void plan_drop(DefragPlan *p)
{if(p){mem_drop(p->files);mem_drop(p->avoid);mem_drop(p);}}
static int plan_impl(void *v,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!live(v))return IFS_FS_INVALID;
    if(!session.writable)return IFS_FS_ACCESS;
    if(active_plan)return IFS_FS_BUSY;
    if(policy!=0u&&policy!=10u)return IFS_FS_UNSUPPORTED;
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
    *p=(DefragPlan){.policy=policy};memset(seen,0,(total+7u)/8u);
    memcpy(bitmap,session.bitmap,session.g.bitmap_bytes);
    if(policy){
        p->avoid=mem_new(session.g.bitmap_bytes);
        if(!p->avoid){mem_drop(bitmap);mem_drop(seen);plan_drop(p);return IFS_FS_NO_MEMORY;}
        memset(p->avoid,0,session.g.bitmap_bytes);
    }
    rc=plan_collect(p,seen,root.ino,0);
    if(!rc && policy)rc=growth_preflight(bitmap,p);
    if(!rc && !policy)for(uint32_t i=0;i<p->count;i++){
        rc=simulate_file(bitmap,&p->files[i]);if(rc)break;
    }
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
    if(!rc && f->growth){
        const Reservation *r=reservation_for(&session,f->ino);
        if(!r||r->count!=f->growth||r->start!=f->start+(f->bytes+BB-1u)/BB)
            rc=IFS_FS_CORRUPT;
    }
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
    Runs data={0},reserve={0};
    uint32_t blocks=(f->bytes+BB-1u)/BB;
    if(f->move)rc=exact_runs(&m,f->start,blocks,1,&data);
    else {
        uint32_t start=0;
        if(!contiguous(&o.old,old,&start)||start!=f->start)rc=IFS_FS_CORRUPT;
    }
    if(!rc && f->growth)rc=exact_runs(&m,f->start+blocks,f->growth,1,&reserve);
    if(rc)goto done;
    if(f->move){
        rc=build_descriptors(&m,&o,&data);if(rc)goto done;
        FileSource source={.old=&o.old,.ext=old,.bytes=f->bytes};
        for(uint32_t i=0;i<blocks;i++){
            if(cancelled(mh())){rc=IFS_FS_BUSY;goto done;}
            uint8_t p[BB]={0};rc=file_source(&source,i,p);if(rc)goto done;
            rc=write_new_block(&m,f->start+i,p);if(rc)goto done;
        }
        rc=release_old(&m,&o.old,old);if(rc)goto done;
        rc=stage_inode(&m,o.old.ino,o.raw);
    }
    if(!rc && f->growth)rc=reservation_create(&m,&o,&reserve,f->growth);
    if(!rc)rc=mutation_commit(&m);
done:
    runs_drop(&data);runs_drop(&reserve);mutation_end(&m);mem_drop(old);return rc;
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
    plan_executing=1;plan_avoid=q->avoid;
    while(q->next<q->count){
        if(cancelled(mh())){rc=IFS_FS_BUSY;break;}
        PlanFile *f=&q->files[q->next];
        if(f->move||f->reserve_only){
            rc=relocate_file(v,f);if(rc)break;
            if(f->move)q->moved+=f->bytes;
        }
        rc=verify_file(f);if(rc){session.fault=1;break;}
        ++q->next;
    }
    plan_executing=0;plan_avoid=NULL;
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
    uint64_t errors=0;int rc=check_impl(v,0,&errors);
    return rc?rc:errors?IFS_FS_CORRUPT:IFS_FS_OK;
}
static void release_impl(void *v,void *p)
{
    if(!v||(uintptr_t)v!=session.token||!p||(uintptr_t)p!=active_plan)return;
    plan_drop(plan_state);plan_state=NULL;active_plan=0;
}
