/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent media interoperability harness. Only the test host decodes
 * the SGI partition table; the provider sees partition-relative Common V2.
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
#include <sys/stat.h>
typedef struct {int fd;uint64_t base,size;unsigned writes;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%512==0&&n%512==0&&at<=d->size&&n<=d->size-at);
    return pread(d->fd,p,n,(off_t)(d->base+at))==(ssize_t)n?0:-1;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Disk *d=ctx;assert(at%512==0&&n%512==0&&at<=d->size&&n<=d->size-at);
    ++d->writes;return pwrite(d->fd,p,n,(off_t)(d->base+at))==(ssize_t)n?0:-1;
}
static int fl(void *ctx){return fsync(((Disk*)ctx)->fd);}
static void *al(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void fr(void *ctx,void *p){(void)ctx;free(p);}
static uint32_t be32(const unsigned char *p){
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static IfsFsHostRuntimeV2 host(Disk *d,int writable){
    return (IfsFsHostRuntimeV2){.abi_version=2,.structure_bytes=sizeof(IfsFsHostRuntimeV2),
      .capabilities=writable?(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE):IFS_FS_HOST_READ_ONLY,
      .volume_bytes=d->size,.logical_sector_bytes=512,.physical_sector_bytes=512,
      .maximum_transfer_bytes=65536,.context=d,.read_at=rd,.write_at=writable?wr:NULL,
      .flush=writable?fl:NULL,.allocate=al,.release=fr};
}
static void locate(Disk *d){
    unsigned char h[512];assert(pread(d->fd,h,512,0)==512);
    if(be32(h)!=0x0be5a941u)return;
    uint32_t sum=0;for(unsigned i=0;i<512;i+=4)sum+=be32(h+i);
    assert(!sum);
    for(unsigned i=0;i<16;i++){
        const unsigned char *p=h+312+12*i;
        uint64_t n=(uint64_t)be32(p)*512,start=(uint64_t)be32(p+4)*512;
        if(!n||be32(p+8)!=7)continue;
        assert(start<=d->size&&n<=d->size-start);
        unsigned char sb[512];assert(pread(d->fd,sb,512,(off_t)(start+512))==512);
        if(be32(sb+28)==0x72959u||be32(sb+28)==0x7295au){
            d->base=start;d->size=n;return;
        }
    }
    assert(!"SGI disk lacks an EFS partition");
}
static unsigned char *input(const char *root,const char *path,uint32_t *size){
    char name[4096];int n=snprintf(name,sizeof name,"%s%s",root,path);assert(n>0&&(size_t)n<sizeof name);
    int fd=open(name,O_RDONLY);assert(fd>=0);struct stat st;assert(fstat(fd,&st)==0&&st.st_size>=0&&st.st_size<=1048576);
    *size=(uint32_t)st.st_size;unsigned char *p=malloc(*size?*size:1);assert(p);
    assert(read(fd,p,*size)==(ssize_t)*size&&close(fd)==0);return p;
}
static void compare(Disk *d,const IfsFsCoreOpsV2 *c,const IfsFsDefragOpsV2 *m,void *v,const char *root,const char *path){
    uint32_t bytes;unsigned char *p=input(root,path,&bytes),*q=malloc(bytes?bytes:1);assert(q);
    char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
    assert(c->stat(v,path,&e)==0&&e.type==IFS_FS_ENTRY_FILE);
    assert(c->read(v,path,0,q,bytes)==0&&!memcmp(p,q,bytes));
    uint64_t at=0;
    for(uint32_t i=0;;i++){
        IfsFsExtentV2 x;uint32_t n=0;int rc=m->map(v,path,i,&x,1,&n);
        if(rc==IFS_FS_END){assert(!n);break;}
        assert(!rc&&n==1&&x.logical_offset==at&&x.length<=bytes-at);
        assert(pread(d->fd,q,(size_t)x.length,(off_t)(d->base+x.physical_offset))==(ssize_t)x.length);
        assert(!memcmp(p+at,q,(size_t)x.length));at+=x.length;
    }
    assert(at==bytes);free(p);free(q);
}
int main(int argc,char **argv){
    assert(argc==5);
    int writing=!strcmp(argv[2],"write");assert(writing||!strcmp(argv[2],"read"));
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    const IfsFsAllocationOpsV2 *a=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    Disk d={.fd=open(argv[3],writing?O_RDWR|O_CREAT|O_TRUNC:O_RDONLY,0600)};assert(d.fd>=0);
    if(writing){d.size=16u*1024*1024;assert(ftruncate(d.fd,(off_t)d.size)==0);}
    else {struct stat st;assert(fstat(d.fd,&st)==0&&st.st_size>=1024);d.size=(uint64_t)st.st_size;locate(&d);}
    IfsFsHostRuntimeV2 h=host(&d,writing);
    void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    const char *paths[]={"/nested/payload.bin","/guard.bin","/empty"};
    if(writing){
        assert(vol->format(&h,0x72959,"ORACLE",0)==0);
        assert(c->mount(&h,&v,&info)==0);
        assert(c->create(v,"/nested",IFS_FS_ENTRY_DIRECTORY)==0);
        for(unsigned i=0;i<3;i++){
            uint32_t bytes;unsigned char *p=input(argv[4],paths[i],&bytes);
            assert(c->create(v,paths[i],IFS_FS_ENTRY_FILE)==0);
            if(bytes)assert(c->write(v,paths[i],0,p,bytes)==0);
            free(p);
        }
        void *plan=NULL;uint64_t moved=0;
        assert(m->plan(v,paths[0],10,&plan)==0&&plan);
        assert(m->execute(v,plan,&moved)==0&&m->verify(v,plan)==0);
        m->release_plan(v,plan);assert(c->unmount(v)==0);
    }else{
        IfsFsHostRuntimeV2 denied=host(&d,1);
        assert(c->mount(&denied,&v,&info)==IFS_FS_ACCESS&&!v&&!d.writes);
    }
    h=host(&d,0);uint32_t variant=0;assert(c->probe(&h,&variant)==0);
    int rc=c->mount(&h,&v,&info);fprintf(stderr,"EFS oracle mount rc=%d partition=%llu bytes=%llu\n",rc,(unsigned long long)d.base,(unsigned long long)d.size);assert(!rc);
    for(unsigned i=0;i<3;i++)compare(&d,c,m,v,argv[4],paths[i]);
    uint64_t at=0,unknown=0,free_bytes=0;
    for(;;){
        IfsFsAllocationRangeV2 ranges[32];uint32_t n=0;uint64_t next=at;
        rc=a->map(v,at,ranges,32,&n);
        assert(ifs_fs_allocation_page_v2_valid(d.size,at,rc,ranges,32,n,&next,&unknown));
        if(rc==IFS_FS_END)break;
        for(uint32_t i=0;i<n;i++)if(ranges[i].state==IFS_FS_SPACE_FREE)free_bytes+=ranges[i].length;
        at=next;
    }
    assert(at==d.size&&!unknown&&free_bytes==info.free_bytes);
    uint64_t errors=99;rc=vol->check(v,0,&errors);
    fprintf(stderr,"EFS oracle check rc=%d errors=%llu\n",rc,(unsigned long long)errors);
    assert(!rc&&!errors);assert(c->unmount(v)==0);
    if(!writing)assert(!d.writes);
    assert(close(d.fd)==0);dlclose(so);
    puts(writing?"EFS provider media export fixture PASS":"EFS independent foreign media PASS");return 0;
}
