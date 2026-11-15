/*
 * vdi_ftstdlib.h - the C library interface used by the FreeType build
 *
 * This replaces freetype/config/ftstdlib.h.  FreeType is compiled with
 * 32-bit int and never sees the EmuTOS C library, which is built with
 * 16-bit int: the functions below are provided by vdi/vdi_ftlibc.c.
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_FTSTDLIB_H_
#define VDI_FTSTDLIB_H_

#include <stddef.h>
#include <limits.h>
#include <stdarg.h>

#define ft_ptrdiff_t  ptrdiff_t

#define FT_CHAR_BIT    CHAR_BIT
#define FT_USHORT_MAX  USHRT_MAX
#define FT_INT_MAX     INT_MAX
#define FT_INT_MIN     INT_MIN
#define FT_UINT_MAX    UINT_MAX
#define FT_LONG_MIN    LONG_MIN
#define FT_LONG_MAX    LONG_MAX
#define FT_ULONG_MAX   ULONG_MAX
/*
 * FT_LLONG_MAX, FT_LLONG_MIN and FT_ULLONG_MAX are deliberately not
 * defined: FreeType then uses its 32-bit arithmetic instead of 64-bit
 * integers, which would need libgcc support the VDI does not link.
 */

void *memchr(const void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
char *strcat(char *dest, const char *src);
int strcmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
size_t strlen(const char *s);
int strncmp(const char *s1, const char *s2, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

#define ft_memchr   memchr
#define ft_memcmp   memcmp
#define ft_memcpy   memcpy
#define ft_memmove  memmove
#define ft_memset   memset
#define ft_strcat   strcat
#define ft_strcmp   strcmp
#define ft_strcpy   strcpy
#define ft_strlen   strlen
#define ft_strncmp  strncmp
#define ft_strncpy  strncpy
#define ft_strrchr  strrchr
#define ft_strstr   strstr

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
#define ft_qsort  qsort

long strtol(const char *nptr, char **endptr, int base);
#define ft_strtol  strtol

/*
 * setjmp/longjmp: the buffer holds d2-d7, a2-a6, the stack pointer and
 * the return address.
 */
typedef long vdi_ft_jmp_buf[16];
#define ft_jmp_buf     vdi_ft_jmp_buf
int ft_setjmp(volatile long *buf) __attribute__((returns_twice));
void ft_longjmp(volatile long *buf, int val) __attribute__((noreturn));

#endif /* VDI_FTSTDLIB_H_ */
