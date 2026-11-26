/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cold interruption qualification of every native namespace mutation.
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
enum { PAYLOAD=8192, PATCH=4097 };
static const IfsFsCoreOpsV2 *c;
static const IfsFsVolumeOpsV2 *v;
static const IfsFsDefragOpsV2 *m;
static uint8_t original[PAYLOAD],patch[PATCH],guard[4096];
static const char *source="/a/source",*target="/b/target";
static int operation(void *vol,unsigned op){
    switch(op){
    case 0:return c->create(vol,"/a/new",IFS_FS_ENTRY_DIRECTORY);
    case 1:return c->write(vol,source,7000,patch,sizeof patch);
    case 2:return c->truncate(vol,source,777);
    case 3:return c->rename(vol,source,target);
    case 4:return c->remove(vol,source);
    case 5:return c->rename(vol,"/a","/b/moved");
    default:abort();
    }
}
static int state(void *vol,unsigned op){
    char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
    uint8_t out[PAYLOAD+PATCH],expected[PAYLOAD+PATCH];
    assert(c->read(vol,"/b/guard",0,out,sizeof guard)==0&&!memcmp(out,guard,sizeof guard));
    const char *path=source;int after=0;
    int status=c->stat(vol,source,&e);
    if(op==0){
        int rc=c->stat(vol,"/a/new",&e);
        assert(rc==0||rc==IFS_FS_NOT_FOUND);after=rc==0;
        if(after){assert(e.type==IFS_FS_ENTRY_DIRECTORY);
            assert(c->directory_entry(vol,"/a/new",0,&e)==IFS_FS_END);}
    }
    if(op==3||op==5){
        const char *other=op==3?target:"/b/moved/source";
        int t=c->stat(vol,other,&e);
        assert((status==0&&t==IFS_FS_NOT_FOUND)||(status==IFS_FS_NOT_FOUND&&t==0));
        after=t==0;if(after)path=other;
    }
    if(op==4){
        assert(status==0||status==IFS_FS_NOT_FOUND);after=status==IFS_FS_NOT_FOUND;
        if(after)return 1;
    }
    assert(c->stat(vol,path,&e)==0&&e.type==IFS_FS_ENTRY_FILE);
    uint32_t n=(uint32_t)e.size_bytes;
    memcpy(expected,original,PAYLOAD);
    if(op==1){
        assert(n==PAYLOAD||n==7000u+PATCH);
        after=n!=PAYLOAD;
        if(after)memcpy(expected+7000,patch,sizeof patch);
    }else if(op==2){assert(n==PAYLOAD||n==777);after=n==777;}
    else assert(n==PAYLOAD);
    assert(c->read(vol,path,0,out,n)==0&&!memcmp(out,expected,n));
    return after;
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    for(unsigned i=0;i<PAYLOAD;i++)original[i]=(uint8_t)(i*31u+i/19u);
    for(unsigned i=0;i<PATCH;i++)patch[i]=(uint8_t)(i*17u+13u);
    memset(guard,0x6d,sizeof guard);
    Disk d={.media=calloc(1,SIZE),.durable=calloc(1,SIZE)};
    uint8_t *base=malloc(SIZE);assert(d.media&&d.durable&&base);
    for(d.sector=512;d.sector<=4096;d.sector*=8){
        memset(d.media,0,SIZE);memset(d.durable,0,SIZE);d.events=d.cut=d.mode=0;d.dead=0;
        IfsFsHostRuntimeV2 h=host(&d,1);
        assert(v->format(&h,0x72959,"CUTS",0)==0);
        void *vol=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->create(vol,"/a",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(vol,"/b",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(vol,source,IFS_FS_ENTRY_FILE)==0);
        assert(c->write(vol,source,0,original,PAYLOAD)==0);
        assert(c->create(vol,"/b/guard",IFS_FS_ENTRY_FILE)==0);
        assert(c->write(vol,"/b/guard",0,guard,sizeof guard)==0);
        assert(c->rename(vol,"//a","/a/new")==IFS_FS_INVALID);
        uint64_t errors=99;
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(v->query(vol,&info)==0);
        uint64_t old_free=info.free_bytes;
        assert(c->unmount(vol)==0);
        memcpy(base,d.durable,SIZE);
        for(unsigned op=0;op<6;op++){
            reset(&d,base);
            assert(c->mount(&h,&vol,&info)==0);
            assert(!state(vol,op));
            assert(operation(vol,op)==0);
            unsigned cuts=d.events;assert(cuts>=8);
            assert(state(vol,op));
            assert(v->query(vol,&info)==0);
            uint64_t new_free=info.free_bytes;
            assert(v->check(vol,0,&errors)==0&&!errors);
            assert(c->unmount(vol)==0);
            unsigned recovered=0,committed=0;
            for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
                reset(&d,base);
                assert(c->mount(&h,&vol,&info)==0);
                d.cut=cut;d.mode=mode;
                assert(operation(vol,op)!=IFS_FS_OK && d.dead);
                assert(c->unmount(vol)==0);
                memcpy(d.media,d.durable,SIZE);d.cut=0;d.dead=0;d.events=0;
                IfsFsHostRuntimeV2 ro=host(&d,0);
                int rr=c->mount(&ro,&vol,&info);
                assert(rr==IFS_FS_OK||rr==IFS_FS_BUSY||rr==IFS_FS_CORRUPT);
                if(!rr)assert(c->unmount(vol)==0);
                assert(!d.events);
                assert(c->mount(&h,&vol,&info)==0);
                uint64_t repairs=99;
                assert(m->recover(vol,0,&repairs)==0&&repairs<=1);
                recovered+=(unsigned)repairs;
                int after=state(vol,op);committed+=(unsigned)after;
                assert(v->query(vol,&info)==0&&info.free_bytes==(after?new_free:old_free));
                assert(v->check(vol,0,&errors)==0&&!errors);
                assert(c->unmount(vol)==0);
            }
            assert(recovered && committed && committed<cuts*3u);
            printf("EFS namespace interruption PASS sector=%u op=%u cases=%u committed=%u recovered=%u\n",
                d.sector,op,cuts*3u,committed,recovered);
        }
    }
    free(base);free(d.media);free(d.durable);dlclose(so);return 0;
}
