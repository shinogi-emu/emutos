/*
 * vdi_ftlibc.c - the C library functions that FreeType needs
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * This file is compiled with 32-bit int, like FreeType and the outline
 * font engine (see the Makefile).  The rest of EmuTOS has its own versions
 * of some of these functions, built with 16-bit int and unusable from here.
 * To avoid clashes at link time, the symbols defined in this file and
 * referenced by the 32-bit objects are renamed with vdi_ftlibc.syms.
 *
 * Memory comes from the services in vdi_ftos.h.
 */

#include "vdi_ftos.h"
#include "vdi_ftstdlib.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include <freetype/internal/ftobjs.h>


void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    while (n--)
    {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }

    return NULL;
}


int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1;
    const unsigned char *p2 = s2;

    while (n--)
    {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }

    return 0;
}


void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    if (d < s)
    {
        while (n--)
            *d++ = *s++;
    }
    else
    {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }

    return dest;
}


void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    /* copy longwords when both pointers are aligned the same way */
    if ((n >= 16) && !(((unsigned long)d | (unsigned long)s) & 3))
    {
        unsigned long *dl = (unsigned long *)d;
        const unsigned long *sl = (const unsigned long *)s;

        for ( ; n >= 4; n -= 4)
            *dl++ = *sl++;
        d = (unsigned char *)dl;
        s = (const unsigned char *)sl;
    }

    while (n--)
        *d++ = *s++;

    return dest;
}


void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;

    while (n--)
        *p++ = (unsigned char)c;

    return s;
}


size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;

    return p - s;
}


int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }

    return (unsigned char)*s1 - (unsigned char)*s2;
}


int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
        n--;
    }

    return n ? (unsigned char)*s1 - (unsigned char)*s2 : 0;
}


char *strcpy(char *dest, const char *src)
{
    char *d = dest;

    while ((*d++ = *src++) != '\0')
        ;

    return dest;
}


char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;

    while (n && *src)
    {
        *d++ = *src++;
        n--;
    }
    while (n--)
        *d++ = '\0';

    return dest;
}


char *strcat(char *dest, const char *src)
{
    strcpy(dest + strlen(dest), src);

    return dest;
}


char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for ( ; ; s++)
    {
        if (*s == (char)c)
            last = s;
        if (!*s)
            break;
    }

    return (char *)last;
}


char *strstr(const char *haystack, const char *needle)
{
    size_t len = strlen(needle);

    if (len == 0)
        return (char *)haystack;

    for ( ; *haystack; haystack++)
        if (!strncmp(haystack, needle, len))
            return (char *)haystack;

    return NULL;
}


long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long acc = 0;
    unsigned long limit;
    int neg = 0;
    int any = 0;
    int overflow = 0;
    int c;

    while ((*s == ' ') || ((*s >= '\t') && (*s <= '\r')))
        s++;
    if ((*s == '-') || (*s == '+'))
        neg = (*s++ == '-');
    if (((base == 0) || (base == 16)) && (s[0] == '0') && ((s[1] | 0x20) == 'x'))
    {
        s += 2;
        base = 16;
    }
    if (base == 0)
        base = (s[0] == '0') ? 8 : 10;
    limit = (unsigned long)LONG_MAX + neg;

    for ( ; ; s++)
    {
        c = (unsigned char)*s;
        if ((c >= '0') && (c <= '9'))
            c -= '0';
        else if (((c | 0x20) >= 'a') && ((c | 0x20) <= 'z'))
            c = (c | 0x20) - 'a' + 10;
        else
            break;
        if (c >= base)
            break;
        any = 1;
        if (acc > (limit - c) / base)
            overflow = 1;
        else
            acc = acc * base + c;
    }

    if (endptr)
        *endptr = (char *)(any ? s : nptr);
    if (overflow)
        acc = limit;

    return neg ? -(long)acc : (long)acc;
}


/*
 * count leading zeroes: FreeType uses __builtin_clz(), which the compiler
 * turns into a call to this libgcc function on CPUs without a suitable
 * instruction.  The libgcc linked with EmuTOS does not have it.
 */
int __clzsi2(unsigned int x);
int __clzsi2(unsigned int x)
{
    int n = 0;

    if (x == 0)
        return 32;
    if (!(x & 0xffff0000UL))
    {
        n += 16;
        x <<= 16;
    }
    if (!(x & 0xff000000UL))
    {
        n += 8;
        x <<= 8;
    }
    if (!(x & 0xf0000000UL))
    {
        n += 4;
        x <<= 4;
    }
    if (!(x & 0xc0000000UL))
    {
        n += 2;
        x <<= 2;
    }
    if (!(x & 0x80000000UL))
        n++;

    return n;
}


/*
 * a shell sort: it is not recursive, and has no worst case that
 * could overflow the stack
 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    unsigned char *b = base;
    size_t gap, i, j, k;
    unsigned char *p, *q, t;

    for (gap = nmemb / 2; gap > 0; gap /= 2)
    {
        for (i = gap; i < nmemb; i++)
        {
            for (j = i; j >= gap; j -= gap)
            {
                p = b + (j - gap) * size;
                q = b + j * size;
                if (compar(p, q) <= 0)
                    break;
                for (k = 0; k < size; k++)
                {
                    t = p[k];
                    p[k] = q[k];
                    q[k] = t;
                }
            }
        }
    }
}


/*
 * setjmp()/longjmp() for FreeType's error handling
 *
 * The buffer (see vdi_ftstdlib.h) holds d2-d7/a2-a6 (11 longs), then the
 * stack pointer and the return address.  The user label prefix is
 * needed because this is compiled with -fleading-underscore.
 */
#define STR2(x) #x
#define STR(x)  STR2(x)
#define SYM(x)  STR(__USER_LABEL_PREFIX__) #x

__asm__(
    "       .text\n"
    "       .even\n"
    "       .globl  " SYM(ft_setjmp) "\n"
    SYM(ft_setjmp) ":\n"
    "       move.l  4(%sp),%a0\n"
    "       movem.l %d2-%d7/%a2-%a6,(%a0)\n"
    "       move.l  %sp,44(%a0)\n"
    "       move.l  (%sp),%a1\n"
    "       move.l  %a1,48(%a0)\n"
    "       moveq   #0,%d0\n"
    "       rts\n"
    "\n"
    "       .globl  " SYM(ft_longjmp) "\n"
    SYM(ft_longjmp) ":\n"
    "       move.l  4(%sp),%a0\n"
    "       move.l  8(%sp),%d0\n"
    "       bne     1f\n"
    "       moveq   #1,%d0\n"
    "1:\n"
    "       movem.l (%a0),%d2-%d7/%a2-%a6\n"
    "       move.l  48(%a0),%a1\n"
    "       move.l  44(%a0),%sp\n"
    "       move.l  %a1,(%sp)\n"
    "       rts\n"
);


/*
 * the memory manager given to FreeType: FT_Init_FreeType() calls
 * FT_New_Memory() to get it
 */

static void *ft_alloc_cb(FT_Memory memory, long size)
{
    (void)memory;
    return vdi_ft_alloc(size);
}


static void ft_free_cb(FT_Memory memory, void *block)
{
    (void)memory;
    vdi_ft_free(block);
}


static void *ft_realloc_cb(FT_Memory memory, long cur_size, long new_size,
                           void *block)
{
    (void)memory;
    (void)cur_size;
    return vdi_ft_realloc(block, new_size);
}


/* const, so that it lives in the read-only part of the image */
static const struct FT_MemoryRec_ vdi_memory =
{
    NULL,
    ft_alloc_cb,
    ft_free_cb,
    ft_realloc_cb
};


FT_BASE_DEF(FT_Memory) FT_New_Memory(void)
{
    return (FT_Memory)&vdi_memory;
}


FT_BASE_DEF(void) FT_Done_Memory(FT_Memory memory)
{
    (void)memory;
}
