/*
 * vdi_ftos.c - file and memory services for the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "gemdos.h"
#include "string.h"
#include "cookie.h"
#include "vdi_ftos.h"
#include "vdi_ftmem.h"
#include "vdi_kept.h"
#include "vdi_sleep.h"
#include "vdi_defs.h"

#if CONF_WITH_VDI_OUTLINE_FONTS

#define COOKIE_MINT     0x4d694e54L     /* 'MiNT' */

/* the deepest level of sub-folders searched by vdi_font_dir_scan() */
#define MAX_SCAN_DEPTH  8

/* room for the longest path built while scanning */
#define SCAN_PATH_LEN   128

/*
 * GEMDOS with a trap of its own
 *
 * trap1() keeps its caller's return address and two registers in static
 * variables.  Under a multitasking GEMDOS, a call from the font engine can
 * sleep in a file read while another program's VDI call goes through
 * trap1(), which then returns the first one to the wrong place.  These
 * keep nothing outside the stack.  GEMDOS may change d0-d2 and a0-a2.
 *
 * Under MiNT, a process that wakes up with its stack pointer below its own
 * kernel stack is taken for one that overflowed it, and killed.  So a call
 * that can sleep is not made on the font stack (see vdi_outline.c), but on
 * the stack the VDI call came in on, below where that call stands (from
 * os_stack(), NULL to stay on the stack in use): ON_OS_STACK(n) switches
 * to the stack in operand n, if it is not 0, and OFF_OS_STACK switches
 * back, once the arguments are off the stack.
 */
#define GEMDOS_CLOBBERS "d1", "d2", "a0", "a1", "a2", "cc", "memory"

#define ON_OS_STACK(n)  "move.l  %%sp,%%a0\n\t" \
                        "tst.l   %" #n "\n\t" \
                        "jeq     1f\n\t" \
                        "move.l  %" #n ",%%sp\n\t" \
                        "1:\n\t" \
                        "move.l  %%a0,-(%%sp)\n\t"
#define OFF_OS_STACK    "\n\tmove.l  (%%sp),%%sp"

static void *os_stack(void)
{
    return vdi_other_gemdos() ? outline_caller_stack() : NULL;
}

static LONG gemdos_w(WORD fn)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(2)
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "addq.l  #2,%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

static LONG gemdos_ww(WORD fn, WORD a)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(3)
     "move.w  %2,-(%%sp)\n\t"
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "addq.l  #4,%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "r"(a), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

static LONG gemdos_wl(WORD fn, LONG a)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(3)
     "move.l  %2,-(%%sp)\n\t"
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "addq.l  #6,%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "r"(a), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

static LONG gemdos_wlw(WORD fn, LONG a, WORD b)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(4)
     "move.w  %3,-(%%sp)\n\t"
     "move.l  %2,-(%%sp)\n\t"
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "addq.l  #8,%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "r"(a), "r"(b), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

static LONG gemdos_wlww(WORD fn, LONG a, WORD b, WORD c)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(5)
     "move.w  %4,-(%%sp)\n\t"
     "move.w  %3,-(%%sp)\n\t"
     "move.l  %2,-(%%sp)\n\t"
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "lea     10(%%sp),%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "r"(a), "r"(b), "r"(c), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

static LONG gemdos_wwll(WORD fn, WORD a, LONG b, LONG c)
{
    register LONG ret __asm__("d0");
    VdiSleepState sw;
    void *sp = os_stack();

    vdi_sleep_begin(&sw);
    __asm__ volatile
    (ON_OS_STACK(5)
     "move.l  %4,-(%%sp)\n\t"
     "move.l  %3,-(%%sp)\n\t"
     "move.w  %2,-(%%sp)\n\t"
     "move.w  %1,-(%%sp)\n\t"
     "trap    #1\n\t"
     "lea     12(%%sp),%%sp"
     OFF_OS_STACK
    : "=r"(ret) : "r"(fn), "r"(a), "r"(b), "r"(c), "d"(sp) : GEMDOS_CLOBBERS);

    {
        LONG r = ret;

        vdi_sleep_end(&sw);
        return r;
    }
}

#define os_setdta(dta)              gemdos_wl(0x1a, (LONG)(dta))
#define os_getdta()                 ((void *)gemdos_w(0x2f))
#define os_open(name, mode)         gemdos_wlw(0x3d, (LONG)(name), mode)
#define os_close(handle)            gemdos_ww(0x3e, handle)
#define os_read(handle, count, buf) gemdos_wwll(0x3f, handle, count, (LONG)(buf))
#define os_seek(offset, handle, mode) gemdos_wlww(0x42, offset, handle, mode)
#define os_sfirst(spec, attr)       gemdos_wlw(0x4e, (LONG)(spec), attr)
#define os_snext()                  gemdos_w(0x4f)

/*
 * TRUE when the VDI is called by the operating system itself: under MiNT,
 * a process that the kernel started (its parent is process 0), such as
 * XaAES's AES thread
 *
 * Such a process must not search folders through GEMDOS: when XaAES's AES
 * thread scanned the fonts folder during XaAES's start, XaAES then read
 * its resource file wrongly and gave up.  Reading files and starting
 * FreeType there did not do it; the Fsfirst()/Fsnext() walk alone did.
 */
LONG vdi_caller_is_system(void)
{
    ULONG value;

    if (!cookie_get(COOKIE_MINT, &value))
        return FALSE;

    return gemdos_w(0x10c) == 0;        /* Pgetppid() */
}


LONG vdi_yield(void)
{
    if (!vdi_other_gemdos())
        return FALSE;                   /* EmuTOS's own GEMDOS: nothing else runs */

    gemdos_w(0xff);                     /* Syield() */
    return TRUE;
}


/*
 * the system blocks of the allocator (vdi_ftmem.c), which carves the
 * engine's small blocks from them
 *
 * These are asked for on the font stack, through vdi_kept.c's Mxalloc()
 * and Mfree() traps, which do not switch to the caller's stack as the
 * file calls above do: they must not sleep, and they do not under
 * FreeMiNT or MagiC (see vdi_ftos.h).
 */
void *ftmem_os_alloc(ULONG size)
{
    if (size > 0x7fffffffUL)
        return NULL;

    return vdi_alloc_kept((LONG)size);
}


void ftmem_os_free(void *block)
{
    vdi_free_kept(block);
}


/*
 * The engine's memory, from vdi_ftmem.c, which also refuses it close to
 * the bottom of the font stack (see VDI_FT_STACK_GUARD in vdi_ft2.h)
 */
void *vdi_ft_alloc(ULONG size)
{
    return ftmem_alloc(size);
}


void vdi_ft_free(void *block)
{
    ftmem_free(block);
}


void *vdi_ft_realloc(void *block, ULONG size)
{
    return ftmem_realloc(block, size);
}


/*
 * read a whole file into memory
 *
 * returns NULL if the file cannot be opened or read in full, or is empty
 */
void *vdi_font_file_load(const char *path, ULONG *size)
{
    LONG ret, length;
    WORD fh;
    void *data;

    ret = os_open(path, 0);
    if (ret < 0)
        return NULL;
    fh = (WORD)ret;

    data = NULL;
    length = os_seek(0L, fh, 2);            /* seek to the end */
    if ((length > 0) && (os_seek(0L, fh, 0) == 0))
    {
        data = vdi_ft_alloc(length);
        if (data)
        {
            if (os_read(fh, length, data) == length)
            {
                if (size)
                    *size = length;
            }
            else
            {
                vdi_ft_free(data);
                data = NULL;
            }
        }
    }
    os_close(fh);

    return data;
}


void vdi_font_file_free(void *data)
{
    vdi_ft_free(data);
}


/*
 * check if a file name has the extension of a font file we can load
 */
static BOOL is_font_name(const char *name)
{
    const char *ext = NULL;
    const char *p;
    char c1, c2, c3;

    for (p = name; *p; p++)
        if (*p == '.')
            ext = p;
    if (!ext || (strlen(ext) != 4))
        return FALSE;

    c1 = ext[1] | 0x20;                 /* lowercase the letters */
    c2 = ext[2] | 0x20;
    c3 = ext[3] | 0x20;

    if ((c1 == 't') && (c2 == 't') && ((c3 == 'f') || (c3 == 'c')))
        return TRUE;                    /* .ttf, .ttc */
    if ((c1 == 'o') && (c2 == 't') && (c3 == 'f'))
        return TRUE;                    /* .otf */

    return FALSE;
}


/*
 * scan one folder, and the folders below it
 *
 * 'path' holds the folder name, without a trailing backslash, in a buffer
 * of SCAN_PATH_LEN bytes.  It is extended while scanning, and restored
 * before returning.
 *
 * returns the number of font files reported, or <0 if the top folder
 * cannot be searched
 */
static LONG scan_folder(char *path, WORD len, WORD depth,
                        void (*found)(const char *path, void *ctx), void *ctx)
{
    DTA dta, *olddta;
    LONG count = 0;
    WORD ret, namelen;

    if (len + 5 > SCAN_PATH_LEN)        /* backslash, "*.*" and NUL */
        return -1;

    olddta = os_getdta();
    os_setdta(&dta);

    strcpy(path + len, "\\*.*");
    ret = (WORD)os_sfirst(path, FA_SUBDIR);
    if ((ret < 0) && (depth == 0))
    {
        os_setdta(olddta);
        path[len] = '\0';
        return ret;
    }
    while (ret == 0)
    {
        namelen = strlen(dta.d_fname);
        if ((len + 1 + namelen < SCAN_PATH_LEN) && (dta.d_fname[0] != '.'))
        {
            path[len] = '\\';
            strcpy(path + len + 1, dta.d_fname);

            if (dta.d_attrib & FA_SUBDIR)
            {
                if (depth < MAX_SCAN_DEPTH)
                {
                    LONG n = scan_folder(path, len + 1 + namelen, depth + 1,
                                         found, ctx);
                    if (n > 0)
                        count += n;
                    os_setdta(&dta);    /* the recursion changed the DTA */
                }
            }
            else if (!(dta.d_attrib & FA_VOL) && is_font_name(dta.d_fname))
            {
                found(path, ctx);
                count++;
            }
        }
        ret = (WORD)os_snext();
    }

    path[len] = '\0';
    os_setdta(olddta);

    return count;
}


/*
 * call found() for every font file in a folder and its sub-folders
 *
 * returns the number of files found, or <0 if the folder cannot be
 * searched
 */
LONG vdi_font_dir_scan(const char *dir,
                       void (*found)(const char *path, void *ctx), void *ctx)
{
    char path[SCAN_PATH_LEN];
    WORD len;

    if (strlen(dir) >= SCAN_PATH_LEN - 5)   /* checked before it is a WORD */
        return -1;
    len = strlen(dir);
    strcpy(path, dir);
    if ((len > 0) && (path[len-1] == '\\'))     /* drop a trailing backslash */
        path[--len] = '\0';

    return scan_folder(path, len, 0, found, ctx);
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS */
