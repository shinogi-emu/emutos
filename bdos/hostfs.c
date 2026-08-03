/*
 * hostfs.c - host folder as a GEMDOS drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The drive is claimed one GEMDOS call at a time rather than through a
 * filesystem abstraction, because EmuTOS has no such abstraction and
 * inventing one would mean threading it through the whole FAT path.
 * Anything not claimed here falls through to stock EmuTOS untouched.
 */

#include "emutos.h"
#include "hostfs.h"
#include "hostfs_name.h"
#include "../bios/virtio_9p.h"
#include "../bios/epochtime.h"
#include "tosvars.h"
#include "kprint.h"
#include "gemerror.h"
#include "string.h"
#include "bdosstub.h"
#include "miscutil.h"

#ifdef MACHINE_QEMU_VIRT

static WORD hostfs_on;

/*
 * Whether the search currently open via the DTA belongs to us. Fsfirst
 * decides this from the path being searched; Fsnext has no path of its
 * own to look at; it just continues whatever Fsfirst last decided. Without
 * this, every Fsfirst/Fsnext pair in the system -- not just ones aimed at
 * C: -- would be answered with the host folder listing, since fn alone
 * does not say which drive is being searched.
 */
static WORD hostfs_active;

static char dir_names[HOSTFS_MAX_ENTRIES][HOSTFS_MAX_NAME];
static WORD dir_count;
static WORD dir_pos;

/* Insertion sort on raw host names, byte order. */
static void dir_sort(void)
{
    WORD i, j;
    char tmp[HOSTFS_MAX_NAME];

    for (i = 1; i < dir_count; i++)
    {
        strcpy(tmp, dir_names[i]);
        for (j = i; j > 0 && hostfs_name_compare(dir_names[j - 1], tmp) > 0; j--)
            strcpy(dir_names[j], dir_names[j - 1]);
        strcpy(dir_names[j], tmp);
    }
}

/*
 * Snapshot the drive root. Dot entries are filtered by hostfs_match(),
 * not here.
 *
 * dirbuf and name are static, not locals: this runs on the BDOS's
 * dedicated internal stack (see rwa.S, "fstrt"), which is a couple of
 * KB shared with the rest of the GEMDOS call chain. A 1KB local here
 * blew that stack and corrupted the return chain, which showed up as
 * the whole Fsfirst/Fsnext self-test replaying itself several times
 * before boot could continue.
 */
static UBYTE dirbuf[1024];
static char dirent_name[HOSTFS_MAX_NAME];

static LONG dir_snapshot(void)
{
    ULONG pos, off, next = 0;
    LONG n;

    dir_count = 0;
    dir_pos = 0;

    if (p9_walk(P9_ROOT_FID, 1, 0) != 0)
        return EDRIVE;
    if (p9_lopen(1, P9_O_RDONLY) != 0)
    {
        p9_clunk(1);
        return EDRIVE;
    }

    n = p9_readdir(1, 0, dirbuf, sizeof(dirbuf));
    for (pos = 0; n > 0 && pos < (ULONG)n && dir_count < HOSTFS_MAX_ENTRIES; )
    {
        off = p9_dirent_next(dirbuf, (ULONG)n, pos, dirent_name,
                              sizeof(dirent_name), &next);
        if (off == 0)
            break;
        pos = off;

        strcpy(dir_names[dir_count], dirent_name);
        dir_count++;
    }

    p9_clunk(1);
    dir_sort();

    return 0;
}

void hostfs_init(void)
{
    if (!virtio_9p_present())
        return;

    drvbits |= (1L << HOSTFS_DRIVE);
    hostfs_on = 1;

    KINFO(("hostfs: drive C registered\n"));
}

WORD hostfs_claims(WORD fn, short *pw)
{
    if (!hostfs_on)
        return 0;

    if (fn == 0x4E)                 /* Fsfirst: the path says the drive */
    {
        const char *name = *(const char * const *)&pw[1];
        WORD drv = extract_drive_number(name);

        if (drv < 0)
            drv = run->p_curdrv;

        hostfs_active = (drv == HOSTFS_DRIVE);
        return hostfs_active;
    }

    if (fn == 0x4F)                 /* Fsnext: continue whatever Fsfirst started */
        return hostfs_active;

    return 0;
}

/*
 * Ported from the reference implementation's fsfirst_match(). Matching
 * is against the RAW host name; the clipped 8.3 name is only ever what
 * gets written into the DTA. Listing membership and display names are
 * therefore computed independently, which is why a name can be visible
 * yet unmatchable by the pattern that appears to describe it.
 */
static WORD hostfs_match(const char *pat, const char *name, WORD subdir)
{
    const char *dot = 0;
    const char *p = pat;
    const char *n = name;
    const char *q;

    if (name[0] == '.')
    {
        if (!subdir)
            return 0;                   /* root drops every dot entry */
        if (strcmp(name, ".") && strcmp(name, ".."))
            return 0;
    }

    for (q = name; *q; q++)
        if (*q == '.')
            dot = q;                    /* the LAST dot */

    /* A plain "*" must not match anything carrying an extension. */
    if (dot && p[0] == '*' && p[1] == '\0')
        return 0;

    while (*n)
    {
        if (*p == '*')
        {
            while (*n && n != dot)
                n++;
            p++;
        }
        else if (*p == '?' && *n)
        {
            n++;
            p++;
        }
        else if (toupper(*p++) != toupper(*n++))
        {
            return 0;
        }
    }

    while (p[0] == '*')
        p++;
    if (p[0] == '.' && p[1] == '*')     /* ".*" also matches no extension */
        p += 2;
    while (p[0] == '*')
        p++;

    return p[0] == '\0';
}

/* Fill the caller's DTA from one snapshot entry. */
static LONG hostfs_fill_dta(const char *host)
{
    DTA *dta = (DTA *)run->p_xdta;
    ULONG size, mtime;
    UWORD is_dir, writable;
    char clipped[13];

    if (p9_walk(P9_ROOT_FID, 1, host) != 0)
        return EFILNF;

    if (p9_getattr(1, &size, &is_dir, &mtime, &writable) != 0)
    {
        p9_clunk(1);
        return EFILNF;
    }
    p9_clunk(1);

    hostfs_host_to_atari(host, clipped);
    strcpy(dta->d_fname, clipped);

    dta->d_length = (LONG)size;
    dta->d_attrib = 0;
    if (is_dir)
        dta->d_attrib |= FA_SUBDIR;
    if (!writable)
        dta->d_attrib |= FA_RO;

    epoch_to_dos(mtime, &dta->d_date, &dta->d_time);

    KINFO(("hostfs: dta %s len %ld attr 0x%02x\n",
           clipped, dta->d_length, dta->d_attrib));
    KINFO(("hostfs: date %s date 0x%04x time 0x%04x\n",
           clipped, dta->d_date, dta->d_time));

    return 0;
}

static char dir_pattern[16];

LONG hostfs_dispatch(WORD fn, short *pw)
{
    WORD first_call = (fn == 0x4E);

    if (fn == 0x4E)                     /* Fsfirst */
    {
        const char *spec = *(const char **)&pw[1];
        const char *base;
        LONG rc;

        /* Keep only the final component as the match pattern. */
        base = spec;
        while (*spec)
        {
            if (*spec == '\\' || *spec == '/' || *spec == ':')
                base = spec + 1;
            spec++;
        }
        strcpy(dir_pattern, base);

        rc = dir_snapshot();
        if (rc < 0)
            return rc;

        dir_pos = 0;
        fn = 0x4F;                      /* fall into the Fsnext scan */
    }

    /* Fsnext, and the tail of Fsfirst */
    while (dir_pos < dir_count)
    {
        const char *host = dir_names[dir_pos];
        LONG rc;

        dir_pos++;
        if (!hostfs_match(dir_pattern, host, 0))
            continue;

        rc = hostfs_fill_dta(host);
        if (rc == 0)
        {
            char clipped[13];

            hostfs_host_to_atari(host, clipped);
            KINFO(("hostfs: %s %s\n", first_call ? "fsfirst" : "fsnext", clipped));
        }
        return rc;
    }

    return ENMFIL;
}

#endif /* MACHINE_QEMU_VIRT */
