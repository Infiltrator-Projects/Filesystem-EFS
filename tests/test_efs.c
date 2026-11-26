/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native EFS structure fixture and hostile-image regression. This is not
 * a substitute for independent foreign-media and mutation qualification.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
enum { B=512,SIZE=128*B };
static uint8_t disk[SIZE],baseline[SIZE];
static uint32_t sector=512;
static int fail_reads;
static void be16(uint8_t *p,uint32_t n){p[0]=n>>8;p[1]=n;}
static void be32(uint8_t *p,uint32_t n){be16(p,n>>16);be16(p+2,n);}
static void checksum(void){
    uint32_t c=0;for(unsigned i=0;i<88;i+=2){
        c^=(uint32_t)disk[B+i]*256u+disk[B+i+1];c=(c<<1)|(c>>31);
    }be32(disk+B+88,c);
}
static void extent(uint8_t *p,unsigned b,unsigned n,unsigned l){
    p[0]=0;p[1]=b>>16;p[2]=b>>8;p[3]=b;p[4]=n;p[5]=l>>16;p[6]=l>>8;p[7]=l;
}
static uint8_t *inode(unsigned n){return disk+3*B+n*128;}
static void node(unsigned n,unsigned mode,unsigned size,unsigned count){
    uint8_t *p=inode(n);be16(p,mode);be16(p+2,1);be32(p+8,size);be16(p+28,count);
}
static void slot(unsigned block,unsigned i,unsigned off,unsigned ino,const char *name){
    uint8_t *p=disk+block*B;be16(p,0xbeef);p[2]=off/2;p[3]=i+1;p[4+i]=off/2;
    be32(p+off,ino);p[off+4]=strlen(name);memcpy(p+off+5,name,strlen(name));
}
static void fixture(void){
    memset(disk,0,sizeof disk);uint8_t *s=disk+B;
    be32(s,128);be32(s+4,3);be32(s+8,60);be16(s+12,2);be16(s+18,2);
    be32(s+28,0x72959);memcpy(s+32,"EFS",3);be32(s+44,16);be32(s+48,98);be32(s+52,11);
    checksum();
    node(2,0040755,512,1);extent(inode(2)+32,5,1,0);
    node(3,0100644,513,2);extent(inode(3)+32,6,1,0);extent(inode(3)+40,9,1,1);
    node(4,0040755,512,1);extent(inode(4)+32,10,1,0);
    node(5,0100644,13*B,13);extent(inode(5)+32,11,1,1);
    node(6,0100644,0,0);
    slot(5,0,504,2,".");slot(5,1,496,2,"..");slot(5,2,480,3,"small");
    slot(5,3,468,4,"sub");slot(5,4,456,6,"empty");
    slot(10,0,504,4,".");slot(10,1,496,2,"..");slot(10,2,480,5,"indirect");
    for(unsigned i=0;i<13;i++){
        extent(disk+11*B+i*8,12+2*i,1,i);
        memset(disk+(12+2*i)*B,i+1,B);
    }
    memset(disk+6*B,0xa5,B);disk[9*B]=0x5a;
    memcpy(baseline,disk,SIZE);
}
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    (void)ctx;assert(at%sector==0 && n%sector==0 && at<=SIZE && n<=SIZE-at);
    if(fail_reads)return -1;
    memcpy(p,disk+at,n);return 0;
}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void drop(void *ctx,void *p){(void)ctx;free(p);}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){(void)ctx;(void)at;(void)p;(void)n;abort();}
static int flush(void *ctx){(void)ctx;abort();}
int main(int argc,char **argv){
    assert(argc==2);
    void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *symbol=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(symbol);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&symbol,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    assert(c && vol && m && m->header.capabilities==IFS_FS_DEFRAG_MAP);
    assert(!c->write && !c->create && !vol->format && !m->execute);
    fixture();
    IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,
      .capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=SIZE,
      .logical_sector_bytes=512,.physical_sector_bytes=512,.maximum_transfer_bytes=4096,
      .read_at=rd,.allocate=alloc,.release=drop};
    uint32_t variant=0;void *v=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    assert(c->probe(&h,&variant)==0 && variant==0x72959);
    fail_reads=1;assert(c->probe(&h,&variant)==IFS_FS_IO_ERROR && !variant);fail_reads=0;
    for(sector=512;sector<=4096;sector*=8){
        h.logical_sector_bytes=h.physical_sector_bytes=sector;
        assert(c->mount(&h,&v,&info)==0 && v && info.total_bytes==SIZE);
        char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(c->directory_entry(v,"/",0,&e)==0 && !strcmp(name,"small"));
        assert(c->directory_entry(v,"/",1,&e)==0 && !strcmp(name,"sub") && e.type==IFS_FS_ENTRY_DIRECTORY);
        assert(c->directory_entry(v,"/",2,&e)==0 && !strcmp(name,"empty") && !e.size_bytes);
        assert(c->directory_entry(v,"/",3,&e)==IFS_FS_END);
        assert(c->stat(v,"/sub/indirect",&e)==0 && e.size_bytes==13*B);
        uint8_t buf[13*B];assert(c->read(v,"/small",0,buf,513)==0);
        for(unsigned i=0;i<B;i++)assert(buf[i]==0xa5);
        assert(buf[512]==0x5a);
        assert(c->read(v,"/sub/indirect",0,buf,sizeof buf)==0);
        for(unsigned i=0;i<sizeof buf;i++)assert(buf[i]==i/B+1);
        for(unsigned i=0;i<13;i++){
            IfsFsExtentV2 range;uint32_t n=0;
            assert(m->map(v,"/sub/indirect",i,&range,1,&n)==0 && n==1);
            assert(range.physical_offset==(12+2*i)*B && range.logical_offset==i*B && range.length==B);
        }
        IfsFsExtentV2 range;uint32_t n=99;
        assert(m->map(v,"/sub/indirect",13,&range,1,&n)==IFS_FS_END && !n);
        assert(m->map(v,"/empty",0,&range,1,&n)==IFS_FS_END && !n);
        assert(c->unmount(v)==0 && c->stat(v,"/",&e)==IFS_FS_INVALID);
    }
    sector=512;h.logical_sector_bytes=h.physical_sector_bytes=sector;
    h.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;h.write_at=wr;h.flush=flush;
    assert(c->mount(&h,&v,&info)==IFS_FS_ACCESS && !v);
    h.capabilities=IFS_FS_HOST_READ_ONLY;h.write_at=NULL;h.flush=NULL;
    disk[B+88]^=1;assert(c->probe(&h,&variant)==IFS_FS_CORRUPT && !variant);disk[B+88]^=1;
    disk[5*B+4]=1;assert(c->mount(&h,&v,&info)==IFS_FS_CORRUPT && !v);memcpy(disk,baseline,SIZE);
    assert(c->mount(&h,&v,&info)==0);
    extent(disk+11*B+8,12,1,1);
    uint8_t buf[B];assert(c->read(v,"/sub/indirect",0,buf,B)==IFS_FS_CORRUPT);
    assert(c->unmount(v)==0);memcpy(disk,baseline,SIZE);
    assert(!memcmp(disk,baseline,SIZE));dlclose(so);
    puts("EFS native structure, payload, indirect extent and corruption regressions passed");
    return 0;
}
