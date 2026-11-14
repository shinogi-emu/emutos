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

#if CONF_WITH_VDI_OUTLINE_FONTS

/* the deepest level of sub-folders searched by vdi_font_dir_scan() */
#define MAX_SCAN_DEPTH  8

/* room for the longest path built while scanning */
#define SCAN_PATH_LEN   128

#define COOKIE_MINT     0x4d694e54L     /* 'MiNT' */
#define COOKIE_MAGX     0x4d616758L     /* 'MagX' */

/*
 * Mxalloc() mode used under MiNT and MagiC: prefer TT-RAM, keep the block
 * when its owner terminates (0x4000) and make it accessible from supervisor
 * mode (0x30), as fVDI does for its own memory.
 */
#define MX_KEEP_MODE    (MX_PREFTTRAM | 0x4030)

/*
 * the memory is shared by all programs: the engine keeps its caches (sized
 * fonts, glyphs, font files, FreeType internals) until vdi_ft_term(), so it
 * must not be freed when the program that happened to make the VDI call
 * terminates.
 */
static BOOL other_gemdos(void)
{
    ULONG value;

    return cookie_get(COOKIE_MINT, &value) || cookie_get(COOKIE_MAGX, &value);
}


/*
 * allocate a block that lives until it is freed, whoever frees it
 *
 * Under MiNT or MagiC the mode of Mxalloc() does it.  EmuTOS's own GEMDOS
 * records the calling process as owner of every block and frees them when
 * it terminates, so the block is given to the initial process: that is
 * the one running before the first program starts, and it never terminates.
 * Mfree() does not check the owner, so any process can free the block.
 */
static void *alloc_kept(LONG size)
{
    void *block;

    if (other_gemdos())
    {
        block = (void *)Mxalloc(size, MX_KEEP_MODE);
        if ((LONG)block <= 0)
            return NULL;
        return block;
    }

    block = dos_alloc_anyram(size);
    if (block)
        bdos_keep_block(block);

    return block;
}


/*
 * the system blocks of the allocator (vdi_ftmem.c), which carves the
 * engine's small blocks from them
 */
void *ftmem_os_alloc(ULONG size)
{
    if (size > 0x7fffffffUL)
        return NULL;

    return alloc_kept((LONG)size);
}


void ftmem_os_free(void *block)
{
    dos_free(block);
}


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

    ret = dos_open((char *)path, 0);
    if (ret < 0)
        return NULL;
    fh = (WORD)ret;

    data = NULL;
    length = dos_lseek(fh, 2, 0L);          /* seek to the end */
    if ((length > 0) && (dos_lseek(fh, 0, 0L) == 0))
    {
        data = vdi_ft_alloc(length);
        if (data)
        {
            if (dos_read(fh, length, data) == length)
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
    dos_close(fh);

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

    olddta = dos_gdta();
    dos_sdta(&dta);

    strcpy(path + len, "\\*.*");
    ret = dos_sfirst(path, FA_SUBDIR);
    if ((ret < 0) && (depth == 0))
    {
        dos_sdta(olddta);
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
                    dos_sdta(&dta);     /* the recursion changed the DTA */
                }
            }
            else if (!(dta.d_attrib & FA_VOL) && is_font_name(dta.d_fname))
            {
                found(path, ctx);
                count++;
            }
        }
        ret = dos_snext();
    }

    path[len] = '\0';
    dos_sdta(olddta);

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

    len = strlen(dir);
    if (len >= SCAN_PATH_LEN - 5)
        return -1;
    strcpy(path, dir);
    if ((len > 0) && (path[len-1] == '\\'))     /* drop a trailing backslash */
        path[--len] = '\0';

    return scan_folder(path, len, 0, found, ctx);
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS */
