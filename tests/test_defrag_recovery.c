/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cold interruption qualification of normal and growth relocation and reserve consumption.
 * Test host models durable barriers, failed callbacks and torn page writes.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
enum { SIZE=1024*1024 };
typedef struct {uint8_t *media,*durable;uint32_t sector,events,cut,mode;int dead;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && n<=SIZE-at);
    if(d->dead)return -1;
    memcpy(p,d->media+at,n);return 0;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && n<=SIZE-at);
    if(d->dead)return -1;
    if(++d->events==d->cut){
        if(d->mode==1)memcpy(d->durable+at,p,n/2u);
        if(d->mode==2)memcpy(d->durable+at,p,n);
        d->dead=1;return -1;
    }
    memcpy(d->media+at,p,n);return 0;
}
static int fl(void *ctx){
    Disk *d=ctx;if(d->dead)return -1;
    if(++d->events==d->cut){
        if(d->mode==2)memcpy(d->durable,d->media,SIZE);
        d->dead=1;return -1;
    }
    memcpy(d->durable,d->media,SIZE);return 0;
}
static void *al(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void fr(void *ctx,void *p){(void)ctx;free(p);}
static IfsFsHostRuntimeV2 host(Disk *d,int writable){
    return (IfsFsHostRuntimeV2){.abi_version=2,.structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=writable?(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE):IFS_FS_HOST_READ_ONLY,
        .volume_bytes=SIZE,.logical_sector_bytes=d->sector,.physical_sector_bytes=d->sector,
        .maximum_transfer_bytes=4096,.context=d,.read_at=rd,.write_at=writable?wr:NULL,
        .flush=writable?fl:NULL,.allocate=al,.release=fr};
}
static void reset(Disk *d,const uint8_t *base){
    memcpy(d->media,base,SIZE);memcpy(d->durable,base,SIZE);
    d->events=d->cut=d->mode=0;d->dead=0;
}
enum { PAYLOAD=32768, GUARD=4096, APPEND=1025, MAX_EXTENTS=128 };
static const IfsFsCoreOpsV2 *c;
static const IfsFsVolumeOpsV2 *v;
static const IfsFsDefragOpsV2 *m;
static const IfsFsAllocationOpsV2 *a;
static uint8_t payload[PAYLOAD],addition[APPEND];
static uint32_t be32(const uint8_t *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
typedef struct {IfsFsExtentV2 ext[MAX_EXTENTS];uint32_t count;uint64_t bytes;} Mapping;
static Mapping map_file(void *vol){
    Mapping f={0};
    for(;;){
        assert(f.count<MAX_EXTENTS);
        uint32_t n=0;IfsFsExtentV2 *e=&f.ext[f.count];
        int rc=m->map(vol,"/target",f.count,e,1,&n);
        if(rc==IFS_FS_END){assert(!n);break;}
        assert(rc==0&&n==1&&e->logical_offset==f.bytes&&e->length);
        f.bytes+=e->length;++f.count;
    }
    return f;
}
static int same_map(const Mapping *x,const Mapping *y){
    if(x->count!=y->count||x->bytes!=y->bytes)return 0;
    for(uint32_t i=0;i<x->count;i++)
        if(x->ext[i].physical_offset!=y->ext[i].physical_offset||
           x->ext[i].logical_offset!=y->ext[i].logical_offset||
           x->ext[i].length!=y->ext[i].length)return 0;
    return 1;
}
static void check_reserve(Disk *d,void *vol,uint64_t at,uint64_t bytes,uint32_t state){
    uint64_t end=at+bytes;
    uint32_t bitmap=be32(d->media+512+56);if(!bitmap)bitmap=2;
    while(at<end){
        IfsFsAllocationRangeV2 r;uint32_t n=0;
        assert(a->map(vol,at,&r,1,&n)==0&&n==1&&r.physical_offset==at&&r.state==state&&r.length);
        uint64_t next=at+r.length;if(next>end)next=end;
        if(state==IFS_FS_SPACE_RESERVED||state==IFS_FS_SPACE_ALLOCATED)
            for(uint64_t b=at/512;b<next/512;b++)
                assert(!(d->media[bitmap*512+b/8]&(1u<<(b%8))));
        at=next;
    }
}
static void payload_check(void *vol,const Mapping *map,unsigned appended){
    uint8_t out[PAYLOAD+APPEND],guard[GUARD];char name[32];
    uint32_t bytes=PAYLOAD+(appended?APPEND:0);
    assert(map->bytes==bytes);
    assert(c->read(vol,"/target",0,out,bytes)==0&&!memcmp(out,payload,PAYLOAD));
    if(appended)assert(!memcmp(out+PAYLOAD,addition,APPEND));
    for(unsigned i=1;i<8;i+=2){
        snprintf(name,sizeof name,"/block%02u",i);memset(guard,(int)(i+1u),sizeof guard);
        assert(c->read(vol,name,0,out,sizeof guard)==0&&!memcmp(out,guard,sizeof guard));
    }
}
static unsigned recover_cold(Disk *d,const IfsFsHostRuntimeV2 *h,void **vol,
                             IfsFsVolumeInfoV2 *info,int *corruption_tested,uint32_t journal){
    memcpy(d->media,d->durable,SIZE);d->cut=0;d->dead=0;d->events=0;
    IfsFsHostRuntimeV2 ro=host(d,0);
    int rr=c->mount(&ro,vol,info);
    assert(rr==IFS_FS_OK||rr==IFS_FS_BUSY||rr==IFS_FS_CORRUPT);
    if(!rr)assert(c->unmount(*vol)==0);
    assert(!d->events);
    /* On the first confirmed prepared transaction, corrupt a logged
     * payload. All redo pages must be authenticated before ANY home write. */
    if(rr==IFS_FS_BUSY&&!*corruption_tested){
        uint32_t offset=journal+2u*4096u+123u;
        d->media[offset]^=0x80u;
        assert(c->mount(h,vol,info)==IFS_FS_CORRUPT&&!*vol&&!d->events);
        d->media[offset]^=0x80u;
        assert(!memcmp(d->media,d->durable,SIZE));
        *corruption_tested=1;
    }
    assert(c->mount(h,vol,info)==0);
    uint64_t repairs=99,errors=99;
    assert(m->recover(*vol,0,&repairs)==0&&repairs<=1);
    assert(v->check(*vol,0,&errors)==0&&!errors);
    return (unsigned)repairs;
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    a=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    for(unsigned i=0;i<PAYLOAD;i++)payload[i]=(uint8_t)(i*29u+i/257u);
    for(unsigned i=0;i<APPEND;i++)addition[i]=(uint8_t)(i*13u+71u);
    Disk d={.media=calloc(1,SIZE),.durable=calloc(1,SIZE)};
    uint8_t *base=malloc(SIZE),*grown=malloc(SIZE);assert(d.media&&d.durable&&base&&grown);
    const uint64_t reserve=((PAYLOAD/512u+9u)/10u)*512u,used=((APPEND+511u)/512u)*512u;
    for(d.sector=512;d.sector<=4096;d.sector*=8){
        memset(d.media,0,SIZE);memset(d.durable,0,SIZE);d.events=d.cut=d.mode=0;d.dead=0;
        IfsFsHostRuntimeV2 h=host(&d,1);
        assert(v->format(&h,0x72959,"MOVES",0)==0);
        void *vol=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&vol,&info)==0);
        uint8_t guard[GUARD];char name[32];
        for(unsigned i=0;i<8;i++){
            snprintf(name,sizeof name,"/block%02u",i);memset(guard,(int)(i+1u),sizeof guard);
            assert(c->create(vol,name,IFS_FS_ENTRY_FILE)==0);
            assert(c->write(vol,name,0,guard,sizeof guard)==0);
        }
        for(unsigned i=0;i<8;i+=2){
            snprintf(name,sizeof name,"/block%02u",i);assert(c->remove(vol,name)==0);
        }
        assert(c->create(vol,"/target",IFS_FS_ENTRY_FILE)==0);
        assert(c->write(vol,"/target",0,payload,PAYLOAD)==0);
        Mapping old=map_file(vol);assert(old.count>1);
        assert(v->query(vol,&info)==0);uint64_t old_free=info.free_bytes;
        assert(c->unmount(vol)==0);
        memcpy(base,d.durable,SIZE);
        uint32_t journal=be32(base+512+4)*512u-256u*1024u;
        int corrupted=0;
        for(unsigned policy=0;policy<=10;policy+=10){
            reset(&d,base);assert(c->mount(&h,&vol,&info)==0);
            void *plan=NULL;uint64_t moved=0;
            assert(m->plan(vol,"/target",policy,&plan)==0&&plan&&!d.events);
            assert(m->execute(vol,plan,&moved)==0&&moved==PAYLOAD&&m->verify(vol,plan)==0);
            unsigned cuts=d.events;
            Mapping fresh=map_file(vol);assert(fresh.count==1&&fresh.bytes==PAYLOAD);
            if(policy)check_reserve(&d,vol,fresh.ext[0].physical_offset+PAYLOAD,reserve,IFS_FS_SPACE_RESERVED);
            assert(v->query(vol,&info)==0);uint64_t new_free=info.free_bytes;
            m->release_plan(vol,plan);assert(c->unmount(vol)==0);
            if(policy)memcpy(grown,d.durable,SIZE);
            unsigned recovered=0,committed=0;
            for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
                reset(&d,base);assert(c->mount(&h,&vol,&info)==0);
                assert(m->plan(vol,"/target",policy,&plan)==0&&plan&&!d.events);
                d.cut=cut;d.mode=mode;
                assert(m->execute(vol,plan,&moved)!=IFS_FS_OK&&d.dead);
                m->release_plan(vol,plan);assert(c->unmount(vol)==0);
                recovered+=recover_cold(&d,&h,&vol,&info,&corrupted,journal);
                Mapping got=map_file(vol);
                int after=same_map(&got,&fresh);
                assert(after||same_map(&got,&old));committed+=(unsigned)after;
                payload_check(vol,&got,0);
                if(after&&policy)check_reserve(&d,vol,got.ext[0].physical_offset+PAYLOAD,reserve,IFS_FS_SPACE_RESERVED);
                assert(v->query(vol,&info)==0&&info.free_bytes==(after?new_free:old_free));
                assert(c->unmount(vol)==0);
            }
            assert(recovered&&committed&&committed<cuts*3u);
            printf("EFS relocation interruption PASS sector=%u policy=%u cases=%u committed=%u recovered=%u\n",
                d.sector,policy,cuts*3u,committed,recovered);
        }
        assert(corrupted);
        reset(&d,grown);assert(c->mount(&h,&vol,&info)==0);
        Mapping before=map_file(vol);assert(before.count==1);
        assert(v->query(vol,&info)==0);uint64_t before_free=info.free_bytes;
        assert(c->write(vol,"/target",PAYLOAD,addition,APPEND)==0);
        unsigned cuts=d.events;
        Mapping after=map_file(vol);assert(after.count==1&&after.bytes==PAYLOAD+APPEND);
        assert(after.ext[0].physical_offset==before.ext[0].physical_offset);
        assert(v->query(vol,&info)==0);uint64_t after_free=info.free_bytes;
        assert(c->unmount(vol)==0);
        unsigned recovered=0,committed=0;
        for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
            reset(&d,grown);assert(c->mount(&h,&vol,&info)==0);
            d.cut=cut;d.mode=mode;
            assert(c->write(vol,"/target",PAYLOAD,addition,APPEND)!=IFS_FS_OK&&d.dead);
            assert(c->unmount(vol)==0);
            recovered+=recover_cold(&d,&h,&vol,&info,&corrupted,journal);
            Mapping got=map_file(vol);int changed=same_map(&got,&after);
            assert(changed||same_map(&got,&before));committed+=(unsigned)changed;
            payload_check(vol,&got,(unsigned)changed);
            uint64_t consumed=changed?used:0;
            if(consumed)check_reserve(&d,vol,got.ext[0].physical_offset+PAYLOAD,consumed,IFS_FS_SPACE_ALLOCATED);
            check_reserve(&d,vol,got.ext[0].physical_offset+PAYLOAD+consumed,reserve-consumed,IFS_FS_SPACE_RESERVED);
            assert(v->query(vol,&info)==0&&info.free_bytes==(changed?after_free:before_free));
            assert(c->unmount(vol)==0);
        }
        assert(recovered&&committed&&committed<cuts*3u);
        printf("EFS reserve consumption interruption PASS sector=%u cases=%u committed=%u recovered=%u\n",
            d.sector,cuts*3u,committed,recovered);
    }
    free(base);free(grown);free(d.media);free(d.durable);dlclose(so);return 0;
}
