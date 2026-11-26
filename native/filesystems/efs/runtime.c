/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compiler-generated memory references resolve here even after LTO.
 * Compile this translation unit without LTO; symbols remain hidden.
 */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *dst,const void *src,size_t n)
{
    unsigned char *d=dst;const unsigned char *s=src;
    for(size_t i=0;i<n;i++)d[i]=s[i];
    return dst;
}
void *memmove(void *dst,const void *src,size_t n)
{
    unsigned char *d=dst;const unsigned char *s=src;
    if((uintptr_t)d<=(uintptr_t)s)for(size_t i=0;i<n;i++)d[i]=s[i];
    else while(n){--n;d[n]=s[n];}
    return dst;
}
void *memset(void *dst,int c,size_t n)
{
    unsigned char *d=dst;for(size_t i=0;i<n;i++)d[i]=(unsigned char)c;
    return dst;
}
