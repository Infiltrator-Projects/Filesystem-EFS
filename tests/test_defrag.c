/* SPDX-License-Identifier: GPL-3.0-or-later
 * File-backed EFS fragmentation, real physical relocation and cold verification.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
enum { SIZE=16*1024*1024, PAYLOAD=65536, GUARD=8192 };
typedef struct {int fd;uint32_t sector;unsigned writes;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0&&n%d->sector==0&&at<=SIZE&&n<=SIZE-at);
    return pread(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0&&n%d->sector==0&&at<=SIZE&&n<=SIZE-at);
    ++d->writes;return pwrite(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int fl(void *ctx){return fsync(((Disk*)ctx)->fd);}
static void *al(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void fr(void *ctx,void *p){(void)ctx;free(p);}
static IfsFsHostRuntimeV2 host(Disk *d,int writable){
    return (IfsFsHostRuntimeV2){.abi_version=2,.structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=writable?(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE):IFS_FS_HOST_READ_ONLY,
        .volume_bytes=SIZE,.logical_sector_bytes=d->sector,.physical_sector_bytes=d->sector,
        .maximum_transfer_bytes=65536,.context=d,.read_at=rd,.write_at=writable?wr:NULL,
        .flush=writable?fl:NULL,.allocate=al,.release=fr};
}
static uint32_t be32(const uint8_t *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void prove_reserve(Disk *d,const IfsFsAllocationOpsV2 *a,void *vol,uint64_t first,uint64_t bytes){
    uint64_t at=first,end=first+bytes;
    while(at<end){
        IfsFsAllocationRangeV2 r;uint32_t n=0;
        assert(a->map(vol,at,&r,1,&n)==0&&n==1&&r.physical_offset==at&&r.state==IFS_FS_SPACE_RESERVED);
        assert(r.length<=end-at);at+=r.length;
    }
    uint8_t sb[512];assert(pread(d->fd,sb,512,512)==512);
    uint32_t bitmap=be32(sb+56);if(!bitmap)bitmap=2u;
    for(uint64_t block=first/512;block<end/512;block++){
        uint8_t bit;assert(pread(d->fd,&bit,1,(off_t)bitmap*512+(off_t)(block/8))==1);
        assert(!(bit&(1u<<(block%8))));
    }
}
static unsigned extents(const IfsFsDefragOpsV2 *m,void *v,const char *path,IfsFsExtentV2 *first){
    uint64_t at=0;unsigned count=0;
    for(;;){
        IfsFsExtentV2 e;uint32_t n=99;
        int rc=m->map(v,path,count,&e,1,&n);
        if(rc==IFS_FS_END){assert(!n);break;}
        assert(rc==0&&n==1&&e.logical_offset==at&&e.length);
        if(!count)*first=e;
        ++count;at+=e.length;
    }
    return count;
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    const IfsFsAllocationOpsV2 *a=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    assert(m->header.capabilities==IFS_FS_DEFRAG_ALL&&a&&a->map);
    uint8_t payload[PAYLOAD],readback[PAYLOAD+GUARD*2],guard[GUARD];
    for(unsigned i=0;i<PAYLOAD;i++)payload[i]=(uint8_t)(i*19u+i/251u);
    char path[]="/tmp/efs-defrag-XXXXXX";Disk d={.fd=mkstemp(path)};
    assert(d.fd>=0&&unlink(path)==0&&ftruncate(d.fd,SIZE)==0);
    for(d.sector=512;d.sector<=4096;d.sector*=8)for(unsigned policy=0;policy<=10;policy+=10){
        IfsFsHostRuntimeV2 h=host(&d,1);
        assert(v->format(&h,0x72959,"DEFRAG",0)==0);
        void *volume=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&volume,&info)==0);
        char name[32];
        for(unsigned i=0;i<16;i++){
            snprintf(name,sizeof name,"/block%02u",i);
            memset(guard,(int)(i+1u),sizeof guard);
            assert(c->create(volume,name,IFS_FS_ENTRY_FILE)==0);
            assert(c->write(volume,name,0,guard,sizeof guard)==0);
        }
        for(unsigned i=0;i<16;i+=2){
            snprintf(name,sizeof name,"/block%02u",i);
            assert(c->remove(volume,name)==0);
        }
        assert(c->create(volume,"/fragmented",IFS_FS_ENTRY_FILE)==0);
        assert(c->write(volume,"/fragmented",0,payload,sizeof payload)==0);
        IfsFsExtentV2 old={0},now={0};
        unsigned count=extents(m,volume,"/fragmented",&old);assert(count>1);
        uint64_t errors=99;assert(v->check(volume,0,&errors)==0&&!errors);
        unsigned writes=d.writes;void *plan=NULL;uint64_t moved=0;
        assert(m->plan(volume,"/fragmented",99,&plan)==IFS_FS_UNSUPPORTED&&!plan&&d.writes==writes);
        assert(m->plan(volume,"/fragmented",policy,&plan)==0&&plan&&d.writes==writes);
        assert(c->create(volume,"/blocked",IFS_FS_ENTRY_FILE)==IFS_FS_BUSY&&d.writes==writes);
        assert(c->unmount(volume)==IFS_FS_BUSY);
        assert(m->verify(volume,plan)==IFS_FS_BUSY);
        assert(m->execute(volume,plan,&moved)==0&&moved==sizeof payload);
        assert(m->verify(volume,plan)==0);
        assert(extents(m,volume,"/fragmented",&now)==1&&now.length==sizeof payload);
        assert(now.physical_offset!=old.physical_offset);
        uint64_t reserve_bytes=((PAYLOAD/512+9u)/10u)*512u;
        if(policy)prove_reserve(&d,a,volume,now.physical_offset+PAYLOAD,reserve_bytes);
        m->release_plan(volume,plan);
        assert(m->verify(volume,plan)==IFS_FS_INVALID);
        assert(c->unmount(volume)==0);
        h=host(&d,0);assert(c->mount(&h,&volume,&info)==0);
        assert(extents(m,volume,"/fragmented",&now)==1);
        if(policy)prove_reserve(&d,a,volume,now.physical_offset+PAYLOAD,reserve_bytes);
        assert(c->read(volume,"/fragmented",0,readback,PAYLOAD)==0&&!memcmp(payload,readback,sizeof payload));
        assert(pread(d.fd,readback,PAYLOAD,(off_t)now.physical_offset)==PAYLOAD);
        assert(!memcmp(payload,readback,sizeof payload));
        for(unsigned i=1;i<16;i+=2){
            snprintf(name,sizeof name,"/block%02u",i);
            memset(guard,(int)(i+1u),sizeof guard);
            assert(c->read(volume,name,0,readback,sizeof guard)==0&&!memcmp(guard,readback,sizeof guard));
        }
        assert(v->check(volume,0,&errors)==0&&!errors);
        assert(c->unmount(volume)==0);
        if(policy){
            h=host(&d,1);assert(c->mount(&h,&volume,&info)==0);
            assert(c->create(volume,"/.ifs-growth",IFS_FS_ENTRY_FILE)==IFS_FS_ACCESS);
            assert(c->remove(volume,"/.ifs-growth")==IFS_FS_ACCESS);
            assert(c->create(volume,"/new",IFS_FS_ENTRY_FILE)==0);
            assert(c->write(volume,"/new",0,guard,sizeof guard)==0);
            prove_reserve(&d,a,volume,now.physical_offset+PAYLOAD,reserve_bytes);
            assert(m->plan(volume,"/",10,&plan)==0&&plan);
            assert(m->execute(volume,plan,&moved)==0&&m->verify(volume,plan)==0);
            m->release_plan(volume,plan);
            for(unsigned i=1;i<16;i+=2){
                snprintf(name,sizeof name,"/block%02u",i);
                assert(extents(m,volume,name,&now)==1);
                prove_reserve(&d,a,volume,now.physical_offset+GUARD,((GUARD/512+9u)/10u)*512u);
            }
            /* Append consumes native reserved blocks without moving the
             * original payload. A small overwrite preserves the reserve. */
            assert(extents(m,volume,"/fragmented",&now)==1);
            uint64_t start=now.physical_offset;
            uint8_t addition[1025];memset(addition,0x5e,sizeof addition);
            assert(c->write(volume,"/fragmented",PAYLOAD,addition,sizeof addition)==0);
            assert(extents(m,volume,"/fragmented",&now)==1&&now.physical_offset==start&&now.length==PAYLOAD+sizeof addition);
            prove_reserve(&d,a,volume,start+PAYLOAD+1536,reserve_bytes-1536);
            uint8_t marker=0xa7;
            assert(c->write(volume,"/fragmented",17,&marker,1)==0);
            prove_reserve(&d,a,volume,start+PAYLOAD+1536,reserve_bytes-1536);
            assert(c->read(volume,"/fragmented",0,readback,PAYLOAD+sizeof addition)==0);
            assert(readback[17]==marker);readback[17]=payload[17];
            assert(!memcmp(readback,payload,PAYLOAD)&&!memcmp(readback+PAYLOAD,addition,sizeof addition));
            /* Replenish a partially consumed reservation, then grow to
             * use every newly reserved block; no orphan owner may remain. */
            assert(m->plan(volume,"/fragmented",10,&plan)==0);
            assert(m->execute(volume,plan,&moved)==0&&m->verify(volume,plan)==0);
            m->release_plan(volume,plan);
            assert(extents(m,volume,"/fragmented",&now)==1);
            start=now.physical_offset;
            uint64_t allocated=(PAYLOAD+sizeof addition+511u)/512u*512u;
            uint64_t fresh=((allocated/512u+9u)/10u)*512u;
            prove_reserve(&d,a,volume,start+allocated,fresh);
            assert(c->truncate(volume,"/fragmented",allocated+fresh)==0);
            assert(extents(m,volume,"/fragmented",&now)==1&&now.physical_offset==start&&now.length==allocated+fresh);
            IfsFsAllocationRangeV2 range;uint32_t nr=0;
            assert(a->map(volume,start+allocated,&range,1,&nr)==0&&nr==1&&range.state==IFS_FS_SPACE_ALLOCATED);
            assert(c->read(volume,"/fragmented",0,readback,(uint32_t)(allocated+fresh))==0);
            assert(readback[17]==marker);readback[17]=payload[17];
            assert(!memcmp(readback,payload,PAYLOAD)&&!memcmp(readback+PAYLOAD,addition,sizeof addition));
            for(uint64_t i=PAYLOAD+sizeof addition;i<allocated+fresh;i++)assert(!readback[i]);
            assert(v->check(volume,0,&errors)==0&&!errors);
            assert(c->unmount(volume)==0);h=host(&d,1);assert(c->mount(&h,&volume,&info)==0);
            assert(extents(m,volume,"/fragmented",&now)==1&&now.physical_offset==start&&now.length==allocated+fresh);
            assert(c->remove(volume,"/fragmented")==0);
            assert(v->check(volume,0,&errors)==0&&!errors);
            assert(c->unmount(volume)==0);
        }
        printf("EFS physical relocation PASS sector=%u policy=%u old_extents=%u new_extents=1\n",d.sector,policy,count);
    }
    assert(close(d.fd)==0);dlclose(so);return 0;
}
