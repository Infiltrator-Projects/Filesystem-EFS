/* SPDX-License-Identifier: GPL-3.0-or-later
 * Local byte and ordering primitives. No hosted or platform runtime imports.
 * Also supply the memory symbols that a freestanding compiler may emit
 * for structure assignment. Hidden visibility keeps the ABI export unique.
 */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *dst,const void *src,size_t n);
void *memmove(void *dst,const void *src,size_t n);
void *memset(void *dst,int c,size_t n);
static int memcmp(const void *a,const void *b,size_t n)
{
    const unsigned char *x=a,*y=b;
    for(size_t i=0;i<n;i++)if(x[i]!=y[i])return (int)x[i]-(int)y[i];
    return 0;
}
static size_t strlen(const char *s)
{
    size_t n=0;while(s[n])++n;return n;
}
static int strcmp(const char *a,const char *b)
{
    while(*a&&*a==*b){++a;++b;}
    return (int)(unsigned char)*a-(int)(unsigned char)*b;
}
static char *strrchr(const char *s,int c)
{
    const char *last=NULL;
    do {if((unsigned char)*s==(unsigned char)c)last=s;}while(*s++);
    return (char*)last;
}
static void efs_swap(unsigned char *a,unsigned char *b,size_t width)
{
    for(size_t i=0;i<width;i++){unsigned char t=a[i];a[i]=b[i];b[i]=t;}
}
/* In-place heapsort: bounded O(n log n) comparisons and O(1) storage,
 * even when a corrupted directory supplies adversarial name ordering. */
static void efs_sift(unsigned char *p,size_t root,size_t count,size_t width,
                     int (*cmp)(const void *,const void *))
{
    while(root<count/2){
        size_t child=root*2+1;
        if(child+1<count&&cmp(p+child*width,p+(child+1)*width)<0)++child;
        if(cmp(p+root*width,p+child*width)>=0)return;
        efs_swap(p+root*width,p+child*width,width);root=child;
    }
}
static void qsort(void *base,size_t count,size_t width,
                  int (*cmp)(const void *,const void *))
{
    unsigned char *p=base;
    if(count<2||!width||count>SIZE_MAX/width)return;
    for(size_t i=count/2;i>0;--i)efs_sift(p,i-1,count,width,cmp);
    for(size_t i=count;i>1;--i){
        efs_swap(p,p+(i-1)*width,width);efs_sift(p,0,i-1,width,cmp);
    }
}
