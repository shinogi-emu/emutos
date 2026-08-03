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
#include "biosbind.h"

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

/*
 * The current directory on the drive, always in the canonical form
 * "\" or "\SUB\DEEP" -- see the note in hostfs.h. Only one string is
 * needed because this module owns exactly one drive; the drive EmuTOS
 * keeps in p_curdir[] is untouched, since stock code never reaches C:.
 */
static char hostfs_path[HOSTFS_MAX_PATH];

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

    if (p9_walk(P9_ROOT_FID, HOSTFS_WALK_FID, 0) != 0)
        return EDRIVE;
    if (p9_lopen(HOSTFS_WALK_FID, P9_O_RDONLY) != 0)
    {
        p9_clunk(HOSTFS_WALK_FID);
        return EDRIVE;
    }

    n = p9_readdir(HOSTFS_WALK_FID, 0, dirbuf, sizeof(dirbuf));
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

    p9_clunk(HOSTFS_WALK_FID);
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

static WORD hostfs_resolve_in(ULONG dirfid, const char *atari, char *out)
{
    ULONG diroff = 0;
    LONG n;

    /*
     * Clone rather than open dirfid itself: Tlopen fixes a fid to an
     * open stream, and the caller still needs dirfid walkable so it can
     * descend into whatever this scan finds.
     */
    if (p9_walk(dirfid, HOSTFS_SCAN_FID, 0) != 0)
        return 0;
    if (p9_lopen(HOSTFS_SCAN_FID, P9_O_RDONLY) != 0)
    {
        p9_clunk(HOSTFS_SCAN_FID);
        return 0;
    }

    while ((n = p9_readdir(HOSTFS_SCAN_FID, diroff, lookup_buf,
                           sizeof(lookup_buf))) > 0)
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
                p9_clunk(HOSTFS_SCAN_FID);
                return 1;
            }
        }

        if (!progress)
            break;              /* nothing consumed: stop rather than spin */
    }

    p9_clunk(HOSTFS_SCAN_FID);

    return 0;
}

static WORD hostfs_resolve(const char *atari, char *out)
{
    return hostfs_resolve_in(P9_ROOT_FID, atari, out);
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
 *
 * For the same reason a RELATIVE name is refused whenever Dsetpath has
 * moved the current directory off the root: the file the caller means
 * lives in that directory, and answering with the root's file of the
 * same name would again be the wrong file rather than an error.
 *
 * "." and ".." are refused outright. They are directories, so the
 * is_dir check downstream would catch them, but only by accident --
 * ".." names the host folder's PARENT, which is outside the drive, and
 * nothing outside the drive should ever reach a walk.
 */
static const char *hostfs_rootname(const char *path)
{
    const char *p = path;

    if (p[0] && p[1] == ':')        /* drop a leading drive specifier */
        p += 2;
    if (*p == '\\' || *p == '/')    /* and one root separator */
        p++;
    else if (strcmp(hostfs_path, "\\"))
        return NULL;                /* relative, and we are not at the root */

    for (path = p; *path; path++)
        if (*path == '\\' || *path == '/' || *path == ':')
            return NULL;

    if (!strcmp(p, ".") || !strcmp(p, ".."))
        return NULL;

    return p;
}

static char open_host[HOSTFS_MAX_NAME];

/*
 * The Fopen mode word, reduced to the access code stock EmuTOS acts on.
 *
 * xopen() masks with VALID_FOPEN_BITS (fs.h:166-173) and ixopen() then
 * looks only at "is it zero", so the inheritance bit (0x80) and the
 * share-mode bits (0x70) are ignored rather than rejected
 * (fsopnclo.c:221-226, 259-261). Testing the raw word here instead
 * would make C: stricter than every other drive for callers that set
 * those bits, which is a difference no application expects.
 */
#define HOSTFS_FOPEN_BITS 0x07

static LONG hostfs_open(const char *path, WORD mode)
{
    ULONG fid, size, mtime;
    UWORD is_dir, writable;
    const char *name;
    WORD slot = 0, i;

    mode &= HOSTFS_FOPEN_BITS;      /* 0 = read, 1 = write, 2 = read/write */

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

    /*
     * A write-capable open is accepted, and the fid is still opened
     * read-only on the wire, because this module has no Twrite path
     * yet: opening for read/write and then only reading has to work,
     * since plenty of software asks for mode 2 out of habit and would
     * otherwise fail on C: where it succeeds everywhere else. Fwrite
     * refuses cleanly with EACCDN until stage 2 adds the write path;
     * that is the same error the caller would have got from the open,
     * only later, and it costs nothing to the reader.
     *
     * The one rule kept exactly is stock's: a file the host will not
     * let us write cannot be opened for writing at all, which mirrors
     * ixopen()'s FA_RO test (fsopnclo.c:259-261).
     */
    if (mode != 0 && !writable)
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

/*
 * The GEMDOS attribute byte for a host object. This is the single
 * definition of the rule: hostfs_fill_dta() writes it into the DTA for
 * Fsfirst/Fsnext and hostfs_attrib() returns the same byte to Fattrib,
 * so the two can never disagree about what a file looks like.
 *
 * Nothing sets FA_ARCHIVE: the host has no such bit, and claiming it
 * would tell every backup tool that every file has changed.
 */
static UBYTE hostfs_attr(UWORD is_dir, UWORD writable)
{
    UBYTE attr = 0;

    if (is_dir)
        attr |= FA_SUBDIR;
    if (!writable)
        attr |= FA_RO;

    return attr;
}

/*
 * Current-directory handling.
 *
 * hostfs_path is built and compared textually, never handed to the
 * host. That is the containment guarantee: ".." pops a component off
 * this string and, at the root, is refused outright rather than clamped
 * silently, so no walk is ever issued for a name above the shared
 * folder. Clamping would be friendlier and wrong -- "\..\..\ETC" would
 * then quietly mean "\ETC", and a caller probing for an escape would be
 * told it succeeded.
 *
 * Static buffers, not locals: the BDOS's private stack is small (see
 * the note on dirbuf above).
 */
static char path_work[HOSTFS_MAX_PATH];
static char path_canon[HOSTFS_MAX_PATH];
static char path_comp[HOSTFS_MAX_NAME];
static char path_host[HOSTFS_MAX_NAME];
static char path_clipped[13];

/* Drop the last component. Zero when already at the root. */
static WORD path_pop(char *p)
{
    char *last = NULL;
    char *q;

    if (!strcmp(p, "\\"))
        return 0;               /* already the root: nothing above it */

    for (q = p; *q; q++)
        if (*q == '\\')
            last = q;

    if (!last)
        return 0;

    /*
     * The leading separator is part of the root, not of the component
     * being dropped, so popping "\SUB" has to leave "\" rather than the
     * empty string -- truncating at last would make the next ".." see a
     * path it does not recognise as the root and let it through.
     */
    if (last == p)
        p[1] = '\0';
    else
        *last = '\0';

    return 1;
}

/* Append one component. Zero when it would not fit. */
static WORD path_push(char *p, const char *comp)
{
    WORD len = (WORD)strlen(p);

    if (len + 1 + (WORD)strlen(comp) + 1 > HOSTFS_MAX_PATH)
        return 0;

    if (len == 0 || p[len - 1] != '\\')    /* the root already ends in one */
        p[len++] = '\\';
    strcpy(p + len, comp);

    return 1;
}

/*
 * Reduce PATH to a canonical drive-relative path in OUT, applying "."
 * and ".." textually. Both separators are accepted, since a path can
 * reach GEMDOS from either convention and rejecting '/' here would only
 * move the failure somewhere less obvious.
 */
static LONG path_build(const char *path, char *out)
{
    const char *p = path;

    if (p[0] && p[1] == ':')            /* drop a leading drive specifier */
        p += 2;

    if (*p == '\\' || *p == '/')
        strcpy(out, "\\");              /* absolute: start at the root */
    else
        strcpy(out, hostfs_path);       /* relative: start where we are */

    while (*p)
    {
        WORD n = 0;

        while (*p == '\\' || *p == '/')
            p++;

        while (*p && *p != '\\' && *p != '/')
        {
            if (*p == '*' || *p == '?')     /* xchdir() refuses these too */
                return EPTHNF;
            if (n >= (WORD)sizeof(path_comp) - 1)
                return EPTHNF;
            path_comp[n++] = *p++;
        }
        path_comp[n] = '\0';

        if (n == 0)                     /* doubled or trailing separator */
            continue;
        if (!strcmp(path_comp, "."))
            continue;
        if (!strcmp(path_comp, ".."))
        {
            if (!path_pop(out))
                return EPTHNF;          /* refuse to climb out of the drive */
            continue;
        }
        if (!path_push(out, path_comp))
            return EPTHNF;
    }

    return E_OK;
}

/*
 * Confirm WANT names a real directory, and write the same path spelled
 * in the host's own capitalisation-then-clipped form into CANON.
 *
 * The walk is a component at a time because p9_walk() carries a single
 * name element, and each component has to be resolved through a
 * directory scan first: host names are longer than 8.3, so "LONGDIR"
 * as typed by the caller is not the name the host knows.
 */
static LONG path_validate(const char *want, char *canon)
{
    const char *p = want;
    LONG rc = E_OK;

    strcpy(canon, "\\");

    if (p9_walk(P9_ROOT_FID, HOSTFS_WALK_FID, 0) != 0)
        return EDRIVE;

    while (*p && rc == E_OK)
    {
        ULONG size, mtime;
        UWORD is_dir, writable;
        WORD n = 0;

        while (*p == '\\')
            p++;
        while (*p && *p != '\\' && n < (WORD)sizeof(path_comp) - 1)
            path_comp[n++] = *p++;
        path_comp[n] = '\0';

        if (n == 0)
            break;

        if (!hostfs_resolve_in(HOSTFS_WALK_FID, path_comp, path_host))
            rc = EPTHNF;
        else if (p9_walk(HOSTFS_WALK_FID, HOSTFS_WALK_FID, path_host) != 0)
            rc = EPTHNF;
        else if (p9_getattr(HOSTFS_WALK_FID, &size, &is_dir, &mtime,
                            &writable) != 0)
            rc = EPTHNF;
        else if (!is_dir)
            rc = EPTHNF;        /* a file is not a path */
        else
        {
            hostfs_host_to_atari(path_host, path_clipped);
            if (!path_push(canon, path_clipped))
                rc = EPTHNF;
        }
    }

    p9_clunk(HOSTFS_WALK_FID);

    return rc;
}

static LONG hostfs_setpath(const char *path)
{
    LONG rc;

    rc = path_build(path, path_work);
    if (rc < 0)
        return rc;

    rc = path_validate(path_work, path_canon);
    if (rc < 0)
        return rc;

    strcpy(hostfs_path, path_canon);

    return E_OK;
}

/*
 * Dgetpath returns the path with neither the drive letter nor a
 * trailing separator, so the root is the EMPTY string, not "\". That
 * is what stock EmuTOS produces -- xgetdir() builds "\SUB\" and then
 * overwrites the final separator with a NUL (fsdir.c:1246-1247) -- and
 * software concatenates the result with "\NAME", so returning "\" here
 * would hand it "\\NAME".
 */
static LONG hostfs_getpath(char *buf)
{
    if (!strcmp(hostfs_path, "\\"))
        *buf = '\0';
    else
        strcpy(buf, hostfs_path);

    return E_OK;
}

/*
 * Dfree.
 *
 * 9P2000.L has a statfs, but the host folder's real free space is the
 * wrong answer anyway: it is the free space of a filesystem that may be
 * terabytes, and every field here is a 32-bit cluster count that GEM
 * multiplies out into a LONG. So this reports a fixed 64MB, which is
 * large enough that nothing refuses to write and small enough that
 * free * clsiz * secsiz cannot overflow a signed 32-bit byte count in
 * the caller.
 *
 * Reporting zero would be the honest-looking choice and is the one
 * thing that must not happen: GEM checks free space before a copy and
 * would refuse every write on the drive.
 */
#define HOSTFS_SECSIZ    512L
#define HOSTFS_CLSIZ     2L         /* sectors per cluster: 1KB clusters */
#define HOSTFS_CLUSTERS  65535L     /* ~64MB, free and total alike */

static LONG hostfs_dfree(LONG *buf)
{
    buf[0] = HOSTFS_CLUSTERS;       /* free clusters */
    buf[1] = HOSTFS_CLUSTERS;       /* total clusters */
    buf[2] = HOSTFS_SECSIZ;         /* bytes per sector */
    buf[3] = HOSTFS_CLSIZ;          /* sectors per cluster */

    return E_OK;
}

static char attrib_host[HOSTFS_MAX_NAME];

/*
 * Fattrib, query only.
 *
 * Setting is refused with EACCDN rather than silently accepted: there
 * is no Tsetattr path yet, and pretending the attribute changed would
 * leave the caller believing something about the file that is not true.
 *
 * Folders return EFILNF, which is what TOS 1-3 and stock EmuTOS do --
 * see the commentary above xchmod() (fsdir.c:390-401).
 */
static LONG hostfs_attrib(const char *path, WORD wrt)
{
    ULONG size, mtime;
    UWORD is_dir, writable;
    const char *name;

    name = hostfs_rootname(path);
    if (!name || !*name)
        return EPTHNF;

    if (!hostfs_resolve(name, attrib_host))
        return EFILNF;

    if (p9_walk(P9_ROOT_FID, HOSTFS_WALK_FID, attrib_host) != 0)
        return EFILNF;

    if (p9_getattr(HOSTFS_WALK_FID, &size, &is_dir, &mtime, &writable) != 0)
    {
        p9_clunk(HOSTFS_WALK_FID);
        return EFILNF;
    }
    p9_clunk(HOSTFS_WALK_FID);

    if (is_dir)
        return EFILNF;

    if (wrt)
        return EACCDN;

    return hostfs_attr(is_dir, writable);
}

void hostfs_init(void)
{
    strcpy(hostfs_path, "\\");


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

    /*
     * Calls whose drive is named by a path: an explicit drive letter if
     * there is one, otherwise the process's current drive.
     */
    if (fn == 0x3D                  /* Fopen   */
     || fn == 0x3B                  /* Dsetpath */
     || fn == 0x43)                 /* Fattrib */
    {
        const char *name = *(const char * const *)&pw[1];
        WORD drv = extract_drive_number(name);

        if (drv < 0)
            drv = run->p_curdrv;

        return drv == HOSTFS_DRIVE;
    }

    /*
     * Calls whose drive is a numbered argument, 0 meaning the current
     * one. Claiming only when that resolves to C: is what leaves every
     * other drive to EmuTOS.
     */
    if (fn == 0x36                  /* Dfree:    drive in pw[3] */
     || fn == 0x47)                 /* Dgetpath: drive in pw[3] */
        return (pw[3] ? pw[3] - 1 : run->p_curdrv) == HOSTFS_DRIVE;

    if (fn == 0x0E)                 /* Dsetdrv: only the switch TO C: */
        return pw[1] == HOSTFS_DRIVE;

    if (fn == 0x19)                 /* Dgetdrv: only while C: is current */
        return run->p_curdrv == HOSTFS_DRIVE;

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

    if (p9_walk(P9_ROOT_FID, HOSTFS_WALK_FID, host) != 0)
        return EFILNF;

    if (p9_getattr(HOSTFS_WALK_FID, &size, &is_dir, &mtime, &writable) != 0)
    {
        p9_clunk(HOSTFS_WALK_FID);
        return EFILNF;
    }
    p9_clunk(HOSTFS_WALK_FID);

    hostfs_host_to_atari(host, clipped);
    strcpy(dta->d_fname, clipped);

    dta->d_length = (LONG)size;
    dta->d_attrib = hostfs_attr(is_dir, writable);

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

    /*
     * Dsetdrv and Dgetdrv are claimed only for C: and do exactly what
     * xsetdrv()/xgetdrv() would have done (fsmain.c:420-441). They are
     * handled here rather than left to stock code so that owning the
     * drive is stated in one place instead of resting on the accident
     * that stock code happens to do the right thing for a drive it
     * knows nothing about.
     */
    case 0x0E:                  /* Dsetdrv(drive) */
        run->p_curdrv = HOSTFS_DRIVE;
        return Drvmap();

    case 0x19:                  /* Dgetdrv() */
        return HOSTFS_DRIVE;

    case 0x3B:                  /* Dsetpath(path) */
        return hostfs_setpath(*(const char * const *)&pw[1]);

    case 0x47:                  /* Dgetpath(buf, drive) */
        return hostfs_getpath(*(char * const *)&pw[1]);

    case 0x36:                  /* Dfree(buf, drive) */
        return hostfs_dfree(*(LONG * const *)&pw[1]);

    case 0x43:                  /* Fattrib(path, wrt, attr) */
        return hostfs_attrib(*(const char * const *)&pw[1], pw[3]);
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
