/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual file-backed EFS shared-object namespace mutation and cold checks.
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
enum { SIZE=16*1024*1024, DATA=512*1024 };
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
static void clean(const IfsFsVolumeOpsV2 *v,void *volume){
    uint64_t errors=99;assert(v->check(volume,0,&errors)==0&&!errors);
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    assert(c->header.capabilities==IFS_FS_CORE_ALL);
    uint8_t *expected=malloc(DATA+4096),*actual=malloc(DATA+4096);assert(expected&&actual);
    char file[]="/tmp/efs-namespace-XXXXXX";Disk d={.fd=mkstemp(file)};
    assert(d.fd>=0&&unlink(file)==0&&ftruncate(d.fd,SIZE)==0);
    for(d.sector=512;d.sector<=4096;d.sector*=8){
        IfsFsHostRuntimeV2 h=host(&d,1);
        assert(v->format(&h,0x72959,"WRITE",0)==0);
        void *volume=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&volume,&info)==0);uint64_t free=info.free_bytes;
        assert(c->create(volume,"/left",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(volume,"/right",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(volume,"/left/deep",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(volume,"/left/deep/file",IFS_FS_ENTRY_FILE)==0);
        unsigned writes=d.writes;
        assert(c->create(volume,"/left/deep/file",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS&&d.writes==writes);
        assert(c->remove(volume,"/left")==IFS_FS_BUSY&&d.writes==writes);
        assert(c->rename(volume,"/left","/left/deep/cycle")==IFS_FS_INVALID&&d.writes==writes);
        for(unsigned i=0;i<DATA;i++)expected[i]=(uint8_t)(i*31u+i/511u);
        for(unsigned at=0;at<DATA;at+=65536u)
            assert(c->write(volume,"/left/deep/file",at,expected+at,65536u)==0);
        clean(v,volume);
        uint8_t patch[513];memset(patch,0x7a,sizeof patch);
        assert(c->write(volume,"/left/deep/file",511,patch,sizeof patch)==0);
        memcpy(expected+511,patch,sizeof patch);
        assert(c->truncate(volume,"/left/deep/file",DATA+4096)==0);
        memset(expected+DATA,0,4096);
        assert(c->rename(volume,"/left/deep/file","/left/deep/renamed")==0);
        assert(c->rename(volume,"/left/deep","/right/moved")==0);
        clean(v,volume);
        assert(c->unmount(volume)==0);
        h=host(&d,0);assert(c->mount(&h,&volume,&info)==0);
        assert(c->read(volume,"/right/moved/renamed",0,actual,DATA+4096)==0);
        assert(!memcmp(expected,actual,DATA+4096));
        writes=d.writes;
        assert(c->write(volume,"/right/moved/renamed",0,patch,1)==IFS_FS_ACCESS&&d.writes==writes);
        assert(c->unmount(volume)==0);h=host(&d,1);assert(c->mount(&h,&volume,&info)==0);
        assert(c->truncate(volume,"/right/moved/renamed",17)==0);
        assert(c->read(volume,"/right/moved/renamed",0,actual,17)==0&&!memcmp(actual,expected,17));
        assert(c->remove(volume,"/right/moved/renamed")==0);
        assert(c->remove(volume,"/right/moved")==0);
        assert(c->remove(volume,"/right")==0&&c->remove(volume,"/left")==0);
        /* Force a multi-block native directory and exercise 255-byte names. */
        char name[257];name[0]='/';memset(name+1,'x',255);name[256]=0;
        for(unsigned i=0;i<9;i++){name[1]=(char)('a'+i);assert(c->create(volume,name,IFS_FS_ENTRY_FILE)==0);}
        clean(v,volume);
        for(unsigned i=0;i<9;i++){name[1]=(char)('a'+i);assert(c->remove(volume,name)==0);}
        clean(v,volume);assert(v->query(volume,&info)==0&&info.free_bytes==free);
        assert(c->unmount(volume)==0);
        printf("EFS native writable namespace PASS sector=%u\n",d.sector);
    }
    assert(close(d.fd)==0);free(expected);free(actual);dlclose(so);return 0;
}
