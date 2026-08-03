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

/*
 * Open files. Slot i is reachable as GEMDOS handle
 * HOSTFS_HANDLE_BASE + i and as 9P fid HOSTFS_FID_BASE + i; the two are
 * never allocated separately, so the invariant is simply
 * "slot free <=> fid unbound".
 *
 * size is cached at open time so Fseek can validate against the file
 * length and Fread can clamp to it without an RPC per call, matching
 * what ixread() does with the OFD's length on a real drive.
 */
static struct
{
    WORD  used;
    ULONG pos;
    ULONG size;
} hostfs_files[HOSTFS_MAX_FILES];

/*
 * Slots are handed out round-robin rather than first-free. First-free
 * would make an open/close loop reuse slot 0 forever, which is exactly
 * the case that cannot detect a fid leak; rotating means a repeated
 * open/close walks the whole table and wraps, so the self-test's last
 * iteration is a real test of the slot the first iteration used.
 */
static WORD hostfs_next_slot;

static WORD hostfs_in_range(WORD h)
{
    return h >= HOSTFS_HANDLE_BASE && h < HOSTFS_HANDLE_BASE + HOSTFS_MAX_FILES;
}

/* Case-insensitive equality, nonzero when the two names match. */
static WORD name_equal(const char *a, const char *b)
{
    while (*a && toupper(*a) == toupper(*b))
    {
        a++;
        b++;
    }

    return toupper(*a) == toupper(*b);
}

/*
 * Resolve an Atari 8.3 name to the host name it was clipped from.
 *
 * This deliberately does NOT go through dir_snapshot(). That function
 * owns dir_names/dir_count/dir_pos, which are the single global state
 * of an in-progress Fsfirst/Fsnext scan, and the classic desktop idiom
 * is Fsfirst; { Fopen; Fread; Fclose } Fsnext -- so resolving a name
 * through the snapshot would silently truncate the very listing that
 * produced the name. It reads the directory again into its own buffer
 * instead.
 *
 * The mapping has to run this way round: hostfs_host_to_atari() is
 * lossy (verylongname.txt becomes VERYLONG.TXT), so the 8.3 name the
 * application passes cannot be walked to directly. Every host name is
 * clipped and compared instead, taking the first hit in readdir order,
 * which is the same rule the listing uses -- so anything Fsfirst showed
 * can be opened by the name it showed.
 *
 * Static buffers, not locals: this runs on the BDOS's small private
 * stack (see the note on dirbuf above).
 */
static UBYTE lookup_buf[1024];
static char lookup_name[HOSTFS_MAX_NAME];
static char lookup_clipped[13];

static WORD hostfs_resolve(const char *atari, char *out)
{
    ULONG diroff = 0;
    LONG n;

    if (p9_walk(P9_ROOT_FID, 1, 0) != 0)
        return 0;
    if (p9_lopen(1, P9_O_RDONLY) != 0)
    {
        p9_clunk(1);
        return 0;
    }

    while ((n = p9_readdir(1, diroff, lookup_buf, sizeof(lookup_buf))) > 0)
    {
        ULONG pos, off, next = 0;
        WORD progress = 0;

        for (pos = 0; pos < (ULONG)n; )
        {
            off = p9_dirent_next(lookup_buf, (ULONG)n, pos, lookup_name,
                                 sizeof(lookup_name), &next);
            if (off == 0)
                break;
            pos = off;
            diroff = next;
            progress = 1;

            hostfs_host_to_atari(lookup_name, lookup_clipped);
            if (name_equal(lookup_clipped, atari))
            {
                strcpy(out, lookup_name);
                p9_clunk(1);
                return 1;
            }
        }

        if (!progress)
            break;              /* nothing consumed: stop rather than spin */
    }

    p9_clunk(1);

    return 0;
}

/*
 * The file name within the drive root, or NULL if the path names
 * anything deeper.
 *
 * Only the root is reachable: hostfs_resolve() reads one directory, and
 * p9_walk() carries a single name element. Rejecting a deeper path is
 * not pedantry -- taking the last component and opening it in the root
 * instead would answer C:\SUB\FILE.TXT with the root's own FILE.TXT,
 * which is worse than failing, because the caller gets the wrong file
 * rather than an error. Subdirectory support is a separate change to
 * both the resolver and the walk.
 */
static const char *hostfs_rootname(const char *path)
{
    const char *p = path;

    if (p[0] && p[1] == ':')        /* drop a leading drive specifier */
        p += 2;
    if (*p == '\\' || *p == '/')    /* and one root separator */
        p++;

    for (path = p; *path; path++)
        if (*path == '\\' || *path == '/' || *path == ':')
            return NULL;

    return p;
}

static char open_host[HOSTFS_MAX_NAME];

static LONG hostfs_open(const char *path, WORD mode)
{
    ULONG fid, size, mtime;
    UWORD is_dir, writable;
    const char *name;
    WORD slot = 0, i;

    if (mode != 0)              /* read-only for now; no Twrite path exists */
        return EACCDN;

    name = hostfs_rootname(path);
    if (!name || !*name)
        return EPTHNF;

    /*
     * Claim the slot before touching the wire, so the no-free-slot case
     * has no fid to release, and so the fid about to be walked is known
     * unbound (slot free <=> fid unbound).
     */
    for (i = 0; i < HOSTFS_MAX_FILES; i++)
    {
        slot = (WORD)((hostfs_next_slot + i) % HOSTFS_MAX_FILES);
        if (!hostfs_files[slot].used)
            break;
    }
    if (i == HOSTFS_MAX_FILES)
        return ENHNDL;

    if (!hostfs_resolve(name, open_host))
        return EFILNF;

    fid = HOSTFS_FID_BASE + slot;

    /*
     * A failed Twalk of a single element is all-or-nothing, so newfid
     * was never established and must not be clunked. Every later
     * failure must, or the fid stays bound with no slot pointing at it
     * and that slot is dead until reboot.
     */
    if (p9_walk(P9_ROOT_FID, fid, open_host) != 0)
        return EFILNF;

    if (p9_getattr(fid, &size, &is_dir, &mtime, &writable) != 0)
    {
        p9_clunk(fid);
        return EFILNF;
    }

    if (is_dir)
    {
        p9_clunk(fid);
        return EACCDN;
    }

    if (p9_lopen(fid, P9_O_RDONLY) != 0)
    {
        p9_clunk(fid);
        return EACCDN;
    }

    hostfs_files[slot].used = 1;
    hostfs_files[slot].pos = 0;
    hostfs_files[slot].size = size;
    hostfs_next_slot = (WORD)((slot + 1) % HOSTFS_MAX_FILES);

    return HOSTFS_HANDLE_BASE + slot;
}

static LONG hostfs_close(WORD h)
{
    WORD slot = h - HOSTFS_HANDLE_BASE;
    WORD was_used;

    if (!hostfs_in_range(h))
        return EIHNDL;

    /*
     * Clunk unconditionally, even for a slot that already looks free.
     * Clunking an unbound fid is answered with an Rlerror that p9_clunk
     * discards, so the cost of being wrong here is one wasted message;
     * the cost of skipping it when the slot state is inconsistent is a
     * permanently bound fid.
     */
    was_used = hostfs_files[slot].used;
    p9_clunk(HOSTFS_FID_BASE + slot);
    hostfs_files[slot].used = 0;
    hostfs_files[slot].pos = 0;
    hostfs_files[slot].size = 0;

    return was_used ? E_OK : EIHNDL;
}

static LONG hostfs_read(WORD h, ULONG count, UBYTE *buf)
{
    WORD slot = h - HOSTFS_HANDLE_BASE;
    ULONG avail;
    LONG n;

    if (!hostfs_in_range(h) || !hostfs_files[slot].used)
        return EIHNDL;

    /* Clamp to the file length first, as ixread() does, so EOF matches. */
    avail = (hostfs_files[slot].pos < hostfs_files[slot].size)
          ? hostfs_files[slot].size - hostfs_files[slot].pos : 0;
    if (count > avail)
        count = avail;
    if (count == 0)
        return 0;

    n = p9_read(HOSTFS_FID_BASE + slot, hostfs_files[slot].pos, buf, count);
    if (n < 0)
        return n;               /* still open: the caller may retry or close */

    hostfs_files[slot].pos += (ULONG)n;

    return n;
}

static LONG hostfs_seek(LONG offset, WORD h, WORD mode)
{
    WORD slot = h - HOSTFS_HANDLE_BASE;
    LONG newpos;

    if (!hostfs_in_range(h) || !hostfs_files[slot].used)
        return EIHNDL;

    switch (mode)
    {
    case 0: newpos = offset; break;                             /* from start */
    case 1: newpos = (LONG)hostfs_files[slot].pos + offset; break;
    case 2: newpos = (LONG)hostfs_files[slot].size + offset; break;
    default: return EINVFN;
    }

    /* Stock EmuTOS refuses to seek past EOF (ixlseek); match it. */
    if (newpos < 0 || (ULONG)newpos > hostfs_files[slot].size)
        return ERANGE;

    hostfs_files[slot].pos = (ULONG)newpos;

    return newpos;
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

    if (fn == 0x3D)                 /* Fopen: the path says the drive */
    {
        const char *name = *(const char * const *)&pw[1];
        WORD drv = extract_drive_number(name);

        if (drv < 0)
            drv = run->p_curdrv;

        return drv == HOSTFS_DRIVE;
    }

    /*
     * Handle-bearing calls. Claiming these is a memory-safety
     * requirement, not a routing convenience: osif() resolves a handle
     * with sft[h - NUMSTD] and does NOT bound-check the top end, so a
     * handle of 0x0100 that fell through to stock code would index
     * about 175 entries past the end of a 75-entry array and quietly
     * corrupt whatever follows it.
     *
     * So the test is the RANGE alone -- never whether the slot is in
     * use. Stale, closed and never-allocated handles in the range are
     * claimed too, and answered with EIHNDL here, which is the same
     * error stock EmuTOS would have produced had it been able to look.
     *
     * Fwrite is claimed for exactly that reason although this module
     * cannot write: declining it would be the corrupting case.
     */
    if (fn == 0x3E || fn == 0x3F || fn == 0x40)     /* Fclose/Fread/Fwrite */
        return hostfs_in_range(pw[1]);

    if (fn == 0x42)                                 /* Fseek: handle is pw[3] */
        return hostfs_in_range(pw[3]);

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

    /*
     * Stack layout follows funcs[] in bdosmain.c: pw[0] is the function
     * number and the parameters are the raw words the caller pushed.
     * Take LONGs through *(T *)&pw[n] rather than by casting pw[n],
     * which under -mshort would silently truncate to 16 bits.
     */
    switch (fn)
    {
    case 0x3D:                  /* Fopen(path, mode) */
        return hostfs_open(*(const char * const *)&pw[1], pw[3]);

    case 0x3E:                  /* Fclose(handle) */
        return hostfs_close(pw[1]);

    case 0x3F:                  /* Fread(handle, count, buf) */
        return hostfs_read(pw[1], *(const ULONG *)&pw[2],
                           *(UBYTE * const *)&pw[4]);

    case 0x40:                  /* Fwrite: claimed only so it cannot corrupt sft */
        if (!hostfs_in_range(pw[1])
         || !hostfs_files[pw[1] - HOSTFS_HANDLE_BASE].used)
            return EIHNDL;
        return EACCDN;          /* open, but this module is read-only */

    case 0x42:                  /* Fseek(offset, handle, mode) */
        return hostfs_seek(*(const LONG *)&pw[1], pw[3], pw[4]);
    }

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
