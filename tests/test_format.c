/* SPDX-License-Identifier: GPL-3.0-or-later
 * File-backed native format and independent geometry/bitmap checks.
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
typedef struct {int fd;uint32_t sector;uint64_t size;unsigned writes;} Image;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Image *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && at<=d->size && n<=d->size-at);
    return pread(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Image *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && at<=d->size && n<=d->size-at);
    ++d->writes;return pwrite(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int flush(void *ctx){return fsync(((Image*)ctx)->fd);}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void drop(void *ctx,void *p){(void)ctx;free(p);}
static uint32_t be16(const uint8_t *p){return (uint32_t)p[0]*256u+p[1];}
static uint32_t be32(const uint8_t *p){return (be16(p)<<16)|be16(p+2);}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    IfsFsModuleApiEntryV2 fn=NULL;void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"efs"));
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsAllocationOpsV2 *a=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    const IfsFsFormatProfileOpsV2 *f=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_FORMAT_PROFILE);
    IfsFsFormatProfileV2 p={.structure_bytes=sizeof p};
    assert(f && f->get_default(&p)==0 && ifs_fs_format_profile_v2_valid(&p));
    assert(p.exact_block_bytes==512 && p.layout==IFS_FS_LAYOUT_MULTI_EXTENT);
    char path[]="/tmp/efs-format-XXXXXX";Image d={.fd=mkstemp(path)};
    assert(d.fd>=0 && unlink(path)==0);
    for(unsigned test=0;test<2;test++){
        d.sector=test?4096u:512u;d.size=test?p.recommended_bytes:p.minimum_bytes;
        assert(ftruncate(d.fd,(off_t)d.size)==0);
        IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,
            .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
            .volume_bytes=d.size,.logical_sector_bytes=d.sector,.physical_sector_bytes=d.sector,
            .maximum_transfer_bytes=65536u,.context=&d,.read_at=rd,.write_at=wr,
            .flush=flush,.allocate=alloc,.release=drop};
        unsigned writes=d.writes;
        assert(vol->format(&h,0,"EFS",0)==IFS_FS_UNSUPPORTED && d.writes==writes);
        assert(vol->format(&h,p.format_variant,"TOOLONG",0)==IFS_FS_INVALID && d.writes==writes);
        IfsFsHostRuntimeV2 tiny=h;tiny.volume_bytes=p.minimum_bytes-d.sector;
        assert(vol->format(&tiny,p.format_variant,"EFS",0)==IFS_FS_UNSUPPORTED && d.writes==writes);
        assert(vol->format(&h,p.format_variant,"EFS",0)==0);
        uint8_t sb[512],replica[512],root[512],inode[512];
        assert(pread(d.fd,sb,512,512)==512);
        assert(be32(sb+28)==p.probe_variant && be16(sb+20)==0);
        uint32_t size=be32(sb),first=be32(sb+4),group=be32(sb+8),groups=be16(sb+18);
        uint32_t inodes=be16(sb+12),rb=first+inodes;
        assert(size==first+group*groups && be32(sb+48)==groups*(group-inodes)-1);
        assert(pread(d.fd,replica,512,(off_t)be32(sb+60)*512)==512&&!memcmp(sb,replica,512));
        assert(pread(d.fd,inode,512,(off_t)first*512)==512);
        assert(be16(inode+256)==0040755u && be16(inode+258)==2 && be32(inode+264)==512);
        assert(pread(d.fd,root,512,(off_t)rb*512)==512 && be16(root)==0xbeef && root[3]==2);
        for(unsigned i=0;i<2;i++){
            unsigned off=root[4+i]*2u;assert(be32(root+off)==2 && root[off+4]==i+1);
            assert(root[off+5]=='.');
        }
        h.capabilities=IFS_FS_HOST_READ_ONLY;h.write_at=NULL;h.flush=NULL;
        uint32_t variant=0;void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(core->probe(&h,&variant)==0&&variant==p.probe_variant);
        assert(core->mount(&h,&v,&info)==0 && info.free_bytes==(uint64_t)be32(sb+48)*512);
        uint32_t name_bytes=0;char name[16];assert(vol->label(v,name,sizeof name,&name_bytes)==0&&!strcmp(name,"EFS"));
        IfsFsEntryV2 entry={.structure_bytes=sizeof entry,.name=name,.name_capacity=sizeof name};
        assert(core->directory_entry(v,"/",0,&entry)==IFS_FS_END);
        uint64_t at=0,unknown=0,free=0;
        for(;;){
            IfsFsAllocationRangeV2 ranges[4];uint32_t n=0;uint64_t next=at;
            int rc=a->map(v,at,ranges,4,&n);
            assert(ifs_fs_allocation_page_v2_valid(d.size,at,rc,ranges,4,n,&next,&unknown));
            if(rc==IFS_FS_END)break;
            for(uint32_t i=0;i<n;i++)if(ranges[i].state==IFS_FS_SPACE_FREE)free+=ranges[i].length;
            at=next;
        }
        assert(at==d.size&&!unknown&&free==info.free_bytes);
        assert(core->unmount(v)==0);
        writes=d.writes;
        assert(vol->format(&h,p.format_variant,"EFS",0)==IFS_FS_ACCESS&&d.writes==writes);
    }
    assert(close(d.fd)==0);dlclose(so);
    puts("EFS native file-backed format and allocation-map regressions passed");return 0;
}
