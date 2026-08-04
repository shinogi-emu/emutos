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
#include "../bios/hostfs_link.h"
#include "../bios/epochtime.h"
#include "tosvars.h"
#include "kprint.h"
#include "gemerror.h"
#include "string.h"
#include "fs.h"
#include "bdosstub.h"
#include "miscutil.h"
#include "biosbind.h"

#ifdef MACHINE_QEMU_VIRT

static WORD hostfs_on;

/*
 * Which search a given Fsnext continues is a property of the DTA, not of
 * this module.
 *
 * Fsfirst decides from the path which drive is being searched; Fsnext
 * has no path of its own and must continue whatever Fsfirst started for
 * THAT DTA. A single global "the last Fsfirst was ours" flag is not
 * enough, because searches interleave: an application may hold a scan of
 * C: open across an unrelated Fsfirst on another drive, and the flag
 * would then decline the C: Fsnext that follows.
 *
 * Declining it is not merely a wrong listing, it is memory corruption.
 * The Fsnext reaches stock xsnext(), which reads dt_offset_drive out of
 * the DTA -- a field this module never wrote, so it holds whatever the
 * application left there. ixsnext() masks it to five bits, checks only
 * the TOP of the range and then indexes drvtbl[] with no NULL test
 * (fsdir.c:556-563); on `virt` there is no floppy, so drvtbl[0] is NULL
 * and a zeroed DTA dereferences low memory and issues a real sector
 * read. There is no MMU.
 *
 * So the routing decision is stamped into the DTA itself. The stamp goes
 * in dt_offset_drive, the same field stock xsfirst() sets to -1L to mark
 * an uninitialised DTA, and it is likewise NEGATIVE: xsnext() returns
 * ENMFIL for any dt_offset_drive < 0L before it calls ixsnext()
 * (fsdir.c:662-663), so even a hostfs Fsnext that this module declines
 * for some other reason fails cleanly instead of walking a FAT
 * directory. -1L is stock's value, so -2L is used here to keep the two
 * distinguishable.
 */
#define HOSTFS_DTA_MAGIC  (-2L)

/*
 * The current directory on the drive, always in the canonical form
 * "\" or "\SUB\DEEP" -- see the note in hostfs.h. Only one string is
 * needed because this module owns exactly one drive; the drive EmuTOS
 * keeps in p_curdir[] is untouched, since stock code never reaches C:.
 */
static char hostfs_path[HOSTFS_MAX_PATH];

/*
 * A path as the host helper spells it: '/'-separated, no leading '/',
 * built from the HOST's own names rather than their 8.3 clippings, and
 * the empty string for the drive root. See the note at the top of
 * bios/hostfs_link.h.
 *
 * These are longer than the Atari paths above and necessarily so: a
 * host name can be far longer than the eight-and-three it is shown as,
 * so a path that fits HOSTFS_MAX_PATH as an Atari string can need many
 * times that on the wire. wire_push() refuses to overflow and the call
 * fails with EPTHNF, which is the failure a caller already handles.
 */
static char path_wire[HOSTFS_LINK_MAX_PATH];

/*
 * The open Fsfirst/Fsnext scan. dir_names/dir_count/dir_pos are the
 * snapshot and the cursor into it; dir_wire is the directory they were
 * taken from, so Fsnext can reach it again to fill the DTA; and
 * dir_subdir says whether that directory is below the drive root, which
 * is what decides the fate of the "." and ".." entries in hostfs_match().
 *
 * All of this belongs to the scan alone. Nothing on the Fopen path may
 * write to it: the desktop's idiom is Fsfirst; { Fopen; Fread; Fclose }
 * Fsnext, so a file call that disturbed the snapshot would truncate the
 * very listing that produced the name it was asked to open.
 */
static char dir_names[HOSTFS_MAX_ENTRIES][HOSTFS_MAX_NAME];
static WORD dir_count;
static WORD dir_pos;
static char dir_wire[HOSTFS_LINK_MAX_PATH];
static WORD dir_subdir;

/*
 * Append one HOST name to a wire path. Zero when it would not fit.
 *
 * The root is the empty string, not "/", because the wire forbids a
 * leading separator -- so the separator is inserted only when there is
 * already something to separate from.
 */
static WORD wire_push(char *p, const char *comp)
{
    WORD len = (WORD)strlen(p);
    WORD n = (WORD)strlen(comp);

    if (len + (len ? 1 : 0) + n + 1 > HOSTFS_LINK_MAX_PATH)
        return 0;

    if (len)
        p[len++] = '/';
    strcpy(p + len, comp);

    return 1;
}

/* Drop the last component. At the root this leaves the root. */
static void wire_pop(char *p)
{
    char *last = NULL;
    char *q;

    for (q = p; *q; q++)
        if (*q == '/')
            last = q;

    if (last)
        *last = '\0';
    else
        *p = '\0';
}

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
 * Snapshot the directory named by the wire path DIRWIRE.
 *
 * READDIR returns ONE entry and an opaque cookie per call. It is not
 * batched, deliberately: the 9P transport fetched a page at a time and
 * the guest kept only the low half of an eight-byte cookie, which is
 * harmless on tmpfs (small integers) and fatal on ext4 (a hash whose
 * identifying half lives in the HIGH word) -- every seek landed back at
 * the start and the directory repeated its first page for ever. One
 * entry per call is slower and cannot fail that way. The cookie is
 * carried whole and no arithmetic is done on it beyond asking whether
 * it moved.
 *
 * "." AND ".." ARE SYNTHESISED HERE. The helper never lists them and
 * refuses them on the wire, because a correct guest reduces them
 * textually before a request is built (path_build() below), so their
 * appearance on the wire could only be an escape attempt. GEM still
 * expects to see them in a subdirectory listing, so they are added to
 * the snapshot exactly as the reference implementation adds them. They
 * are added at the root too and dropped there by hostfs_match(), which
 * is the same filtering the 9P listing needed and keeps the rule in one
 * place.
 *
 * Two things are refused rather than tolerated:
 *
 *   - a directory with more entries than the table holds is REPORTED.
 *     The scan keeps reading so the count it prints is the true
 *     shortfall rather than a guess, but it stops storing. A listing
 *     that is short and says so can be acted on; one that is short and
 *     silent looks exactly like an empty folder.
 *
 *   - a cookie that does not move stops the scan THERE. Re-reading one
 *     entry until the table filled would produce a listing of
 *     plausible names in plausible order, which is far harder to
 *     recognise as broken than a short one. The step count is a
 *     second, outer bound for a cookie that advances yet never ends.
 *
 * dirent_name is static, not a local: this runs on the BDOS's dedicated
 * internal stack (see rwa.S, "fstrt"), which is a couple of KB shared
 * with the rest of the GEMDOS call chain. A 1KB local here blew that
 * stack and corrupted the return chain, which showed up as the whole
 * Fsfirst/Fsnext self-test replaying itself several times before boot
 * could continue.
 */
static char dirent_name[HOSTFS_MAX_NAME];

static LONG dir_snapshot(const char *dirwire)
{
    ULONG cookie = 0;
    LONG dropped = 0;
    LONG toolong = 0;
    LONG steps = 0;
    LONG dh;

    dir_count = 0;
    dir_pos = 0;

    dh = hostfs_link_opendir(dirwire);
    if (dh < 0)
        return dh;

    /* See the note above: the host never lists these and never will. */
    strcpy(dir_names[dir_count++], ".");
    strcpy(dir_names[dir_count++], "..");

    for (;;)
    {
        ULONG next = cookie;

        if (hostfs_link_readdir((ULONG)dh, cookie, dirent_name,
                                sizeof(dirent_name), &next) < 0)
            break;              /* ENMFIL is the end of the directory */

        if (next == cookie)
        {
            KINFO(("hostfs: directory cookie did not advance; listing stopped\n"));
            break;
        }
        cookie = next;

        if (++steps > HOSTFS_MAX_DIRSTEPS)
        {
            KINFO(("hostfs: directory read stopped after %d entries\n",
                   (WORD)HOSTFS_MAX_DIRSTEPS));
            break;
        }

        /*
         * An empty name is hostfs_link_readdir() saying the entry was
         * stepped over but does not fit HOSTFS_MAX_NAME. It is counted
         * and reported rather than passed on: a name this module cannot
         * hold is a file the drive cannot show or open, and the one
         * thing that must not happen is for it to disappear quietly.
         */
        if (!dirent_name[0])
        {
            toolong++;
            continue;
        }

        if (dir_count >= HOSTFS_MAX_ENTRIES)
        {
            dropped++;
            continue;
        }

        strcpy(dir_names[dir_count], dirent_name);
        dir_count++;
    }

    hostfs_link_closedir((ULONG)dh);

    if (dropped > 0)
        KINFO(("hostfs: directory has %ld entries, %ld not listed (limit %d)\n",
               (LONG)dir_count + dropped, dropped, (WORD)HOSTFS_MAX_ENTRIES));

    if (toolong > 0)
        KINFO(("hostfs: %ld name(s) longer than %d bytes not listed\n",
               toolong, (WORD)HOSTFS_MAX_NAME - 1));

    dir_sort();

    return 0;
}

/*
 * Open files. Slot i is reachable as GEMDOS handle
 * HOSTFS_HANDLE_BASE + i, and carries the HOST's handle for the same
 * file -- a small integer the helper allocated, opaque here.
 *
 * Unlike the 9P fids this replaces, the host handle is not a function
 * of the slot: the helper hands one out at OPEN and it means nothing
 * until it does. So the invariant is "used <=> handle is live", and
 * every path that clears `used` closes the handle first.
 *
 * size is cached at open time so Fseek can validate against the file
 * length and Fread can clamp to it without an RPC per call, matching
 * what ixread() does with the OFD's length on a real drive.
 */
static struct
{
    WORD  used;
    ULONG handle;
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
 * produced the name. It opens the directory again and reads it into its
 * own buffer instead.
 *
 * The mapping has to run this way round: hostfs_host_to_atari() is
 * lossy (verylongname.txt becomes VERYLONG.TXT), so the 8.3 name the
 * application passes cannot be walked to directly. Every host name is
 * clipped and compared instead, taking the first hit in readdir order,
 * which is the same rule the listing uses -- so anything Fsfirst showed
 * can be opened by the name it showed.
 *
 * BOTH sides are clipped, and the guest side is clipped with
 * hostfs_atari_clip() rather than with the display mapping. TOS itself
 * truncates a name to 8.3 on the way into GEMDOS, so an application
 * that asks for "VERYLONGNAME.TXT" is asking for "VERYLONG.TXT" and the
 * reference implementation opens it; comparing the host's clipped name
 * against the RAW guest string instead would answer EFILNF for a file
 * that both TOS and the reference implementation open. Note the two
 * clippers are deliberately different functions -- the lookup direction
 * splits at the FIRST dot and neither upcases nor substitutes -- which
 * is why this cannot just call hostfs_host_to_atari() twice.
 *
 * Static buffers, not locals: this runs on the BDOS's small private
 * stack (see the note on dirent_name above).
 */
static char lookup_name[HOSTFS_MAX_NAME];
static char lookup_clipped[13];
static char lookup_want[13];

static WORD hostfs_resolve_in(const char *dirwire, const char *atari,
                              char *out)
{
    ULONG cookie = 0;
    LONG steps = 0;
    WORD found = 0;
    LONG dh;

    hostfs_atari_clip(atari, lookup_want);

    dh = hostfs_link_opendir(dirwire);
    if (dh < 0)
        return 0;

    for (;;)
    {
        ULONG next = cookie;

        if (hostfs_link_readdir((ULONG)dh, cookie, lookup_name,
                                sizeof(lookup_name), &next) < 0)
            break;              /* ENMFIL: the name is simply not there */

        /* Same rule as dir_snapshot(): a cookie that has not moved
         * means the next call returns this entry again, so a name that
         * is genuinely absent would be searched for for ever. */
        if (next == cookie || ++steps > HOSTFS_MAX_DIRSTEPS)
            break;
        cookie = next;

        if (!lookup_name[0])
            continue;           /* entry this build cannot represent */

        hostfs_host_to_atari(lookup_name, lookup_clipped);
        if (name_equal(lookup_clipped, lookup_want))
        {
            strcpy(out, lookup_name);
            found = 1;
            break;
        }
    }

    hostfs_link_closedir((ULONG)dh);

    return found;
}

/*
 * Path resolution -- the ONE place where a guest path becomes a host one.
 *
 * Fopen, Fsfirst, Fattrib and Dsetpath all arrive here. A second
 * resolver is exactly how the listing view and the open view drift
 * apart: a directory that can be shown but whose files cannot be
 * opened is what two of them look like from the desktop.
 *
 * There are two phases, and the order between them is the containment
 * guarantee:
 *
 *   1. path_build() reduces the path TEXTUALLY, applying "." and ".."
 *      to a string that never leaves this module. ".." at the root is
 *      refused outright rather than clamped, so no walk is ever issued
 *      for a name above the shared folder. Clamping would be friendlier
 *      and wrong: "\..\..\ETC" would then quietly mean "\ETC", and a
 *      caller probing for an escape would be told it succeeded.
 *   2. path_walk() then walks the host, a component at a time. It runs
 *      only after phase 1 has approved the whole path, so a refused
 *      path costs no RPC at all.
 *
 * Static buffers, not locals: the BDOS's private stack is small (see
 * the note on dirent_name above).
 */
static char path_work[HOSTFS_MAX_PATH];
static char path_canon[HOSTFS_MAX_PATH];
static char path_dir[HOSTFS_MAX_PATH];
static char path_leaf[HOSTFS_MAX_NAME];
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
 * Walk to the directory named by the canonical path WANT, leaving its
 * wire spelling in path_wire and the same path in the host's own
 * capitalisation-then-clipped form in path_canon.
 *
 * On failure path_wire holds a partial walk and must not be used; every
 * caller checks the return before touching it. Nothing has to be
 * released -- unlike a 9P fid, a wire path is a string, so an error
 * return here cannot leak anything.
 *
 * The walk is a component at a time because each component has to be
 * resolved through a directory scan first: host names are longer than
 * 8.3, so "LONGDIR" as typed by the caller is not the name the host
 * knows. Each resolved component is then STATted, which is what proves
 * it is a directory rather than a file -- the helper would happily
 * OPENDIR nothing else, but "a file is not a path" is a GEMDOS rule and
 * belongs here where EPTHNF is the answer.
 */
static LONG path_walk(const char *want)
{
    const char *p = want;
    LONG rc = E_OK;

    strcpy(path_canon, "\\");
    path_wire[0] = '\0';

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

        if (!hostfs_resolve_in(path_wire, path_comp, path_host))
            rc = EPTHNF;
        else if (!wire_push(path_wire, path_host))
            rc = EPTHNF;
        else if (hostfs_link_stat(path_wire, &size, &is_dir, &mtime,
                                  &writable) < 0)
            rc = EPTHNF;
        else if (!is_dir)
            rc = EPTHNF;        /* a file is not a path */
        else
        {
            hostfs_host_to_atari(path_host, path_clipped);
            if (!path_push(path_canon, path_clipped))
                rc = EPTHNF;
        }
    }

    return rc;
}

/*
 * Resolve PATH into the directory holding its final component, plus
 * that component. path_wire is left holding the directory, path_leaf
 * the component and path_canon the directory's canonical spelling.
 *
 * WANT_FILE asks for the checks that make sense only when the final
 * component names one object rather than a search pattern: it must be
 * present, and it must not be "." or "..". Those two are directories,
 * so a downstream is_dir test would catch them, but only by accident --
 * ".." at the root names the host folder's PARENT, and nothing outside
 * the drive should ever reach a walk.
 *
 * The component itself can contain no separator and no drive colon by
 * construction, because the split is at the LAST one of those in the
 * path: everything structural ends up in the directory part, which
 * phase 1 has already reduced and approved.
 */
static LONG hostfs_locate(const char *path, WORD want_file)
{
    const char *base = path;
    const char *p;
    WORD n;
    LONG rc;

    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;

    if (strlen(base) >= sizeof(path_leaf))
        return EPTHNF;
    strcpy(path_leaf, base);

    if (want_file && (!path_leaf[0]
                   || !strcmp(path_leaf, ".") || !strcmp(path_leaf, "..")))
        return EPTHNF;

    /* Measured as a LONG: a pointer difference does not fit a WORD. */
    if (base - path >= (LONG)sizeof(path_dir))
        return EPTHNF;
    n = (WORD)(base - path);
    memcpy(path_dir, path, n);
    path_dir[n] = '\0';

    rc = path_build(path_dir, path_work);
    if (rc < 0)
        return rc;

    return path_walk(path_work);
}

static char open_host[HOSTFS_MAX_NAME];
static char open_wire[HOSTFS_LINK_MAX_PATH];

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
    ULONG size, mtime;
    UWORD is_dir, writable;
    WORD slot = 0, i;
    LONG rc, handle;

    mode &= HOSTFS_FOPEN_BITS;      /* 0 = read, 1 = write, 2 = read/write */

    rc = hostfs_locate(path, 1);
    if (rc < 0)
        return rc;

    /*
     * Pick the slot first, but do not mark it used until the file is
     * actually open: a slot is never left marked used with no handle in
     * it. Nothing needs releasing on the early returns below, because
     * the host handle does not exist until OPEN succeeds and OPEN is
     * the last thing that happens.
     */
    for (i = 0; i < HOSTFS_MAX_FILES; i++)
    {
        slot = (WORD)((hostfs_next_slot + i) % HOSTFS_MAX_FILES);
        if (!hostfs_files[slot].used)
            break;
    }
    if (i == HOSTFS_MAX_FILES)
        return ENHNDL;

    if (!hostfs_resolve_in(path_wire, path_leaf, open_host))
        return EFILNF;

    strcpy(open_wire, path_wire);
    if (!wire_push(open_wire, open_host))
        return EPTHNF;

    if (hostfs_link_stat(open_wire, &size, &is_dir, &mtime, &writable) < 0)
        return EFILNF;

    /*
     * The helper refuses to OPEN a directory too, but it answers
     * EACCDN for its own reasons; deciding it here keeps the rule where
     * the rest of the GEMDOS semantics live.
     */
    if (is_dir)
        return EACCDN;

    /*
     * A write-capable open is accepted, and the file is still opened
     * read-only on the wire, because this module has no write path yet:
     * opening for read/write and then only reading has to work, since
     * plenty of software asks for mode 2 out of habit and would
     * otherwise fail on C: where it succeeds everywhere else. Fwrite
     * refuses cleanly with EACCDN until the write path lands; that is
     * the same error the caller would have got from the open, only
     * later, and it costs nothing to the reader.
     *
     * The one rule kept exactly is stock's: a file the host will not
     * let us write cannot be opened for writing at all, which mirrors
     * ixopen()'s FA_RO test (fsopnclo.c:259-261).
     */
    if (mode != 0 && !writable)
        return EACCDN;

    handle = hostfs_link_open(open_wire, HOSTFS_LINK_O_RDONLY);
    if (handle < 0)
        return EACCDN;

    hostfs_files[slot].used = 1;
    hostfs_files[slot].handle = (ULONG)handle;
    hostfs_files[slot].pos = 0;
    hostfs_files[slot].size = size;
    hostfs_next_slot = (WORD)((slot + 1) % HOSTFS_MAX_FILES);

    return HOSTFS_HANDLE_BASE + slot;
}

static LONG hostfs_close(WORD h)
{
    WORD slot = h - HOSTFS_HANDLE_BASE;

    if (!hostfs_in_range(h))
        return EIHNDL;

    /*
     * Only a slot that holds a handle is closed. This is the opposite
     * of what the 9P transport did, and deliberately: a fid was a fixed
     * function of the slot, so clunking one that might already be free
     * cost a wasted message and nothing else. A host handle is
     * allocated by the helper and reused after it is closed, so closing
     * a slot that does not hold one risks closing a file some LATER
     * open is using.
     */
    if (!hostfs_files[slot].used)
        return EIHNDL;

    hostfs_link_close(hostfs_files[slot].handle);

    hostfs_files[slot].used = 0;
    hostfs_files[slot].handle = 0;
    hostfs_files[slot].pos = 0;
    hostfs_files[slot].size = 0;

    return E_OK;
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

    n = hostfs_link_read(hostfs_files[slot].handle, hostfs_files[slot].pos,
                         buf, count);
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

static LONG hostfs_setpath(const char *path)
{
    LONG rc;

    rc = path_build(path, path_work);
    if (rc < 0)
        return rc;

    rc = path_walk(path_work);
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
 * The protocol has a DFREE, but the host folder's real free space is
 * the wrong answer anyway: it is the free space of a filesystem that may be
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
static char attrib_wire[HOSTFS_LINK_MAX_PATH];

/*
 * Fattrib, query only.
 *
 * Setting is refused with EACCDN rather than silently accepted: the
 * SETATTR path is not wired up yet, and pretending the attribute
 * changed would leave the caller believing something about the file
 * that is not true.
 *
 * Folders return EFILNF, which is what TOS 1-3 and stock EmuTOS do --
 * see the commentary above xchmod() (fsdir.c:390-401).
 */
static LONG hostfs_attrib(const char *path, WORD wrt)
{
    ULONG size, mtime;
    UWORD is_dir, writable;
    LONG rc;

    rc = hostfs_locate(path, 1);
    if (rc < 0)
        return rc;

    if (!hostfs_resolve_in(path_wire, path_leaf, attrib_host))
        return EFILNF;

    strcpy(attrib_wire, path_wire);
    if (!wire_push(attrib_wire, attrib_host))
        return EPTHNF;

    if (hostfs_link_stat(attrib_wire, &size, &is_dir, &mtime, &writable) < 0)
        return EFILNF;

    if (is_dir)
        return EFILNF;

    if (wrt)
        return EACCDN;

    return hostfs_attr(is_dir, writable);
}

void hostfs_init(void)
{
    strcpy(hostfs_path, "\\");
    dir_wire[0] = '\0';

    if (!hostfs_link_present())
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

        return drv == HOSTFS_DRIVE;
    }

    /*
     * Fsnext continues the search belonging to THIS DTA, which is ours
     * only if our own Fsfirst stamped it -- see HOSTFS_DTA_MAGIC above.
     */
    if (fn == 0x4F)
        return ((DTAINFO *)run->p_xdta)->dt_offset_drive == HOSTFS_DTA_MAGIC;

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

        if (drv >= 0)
            return drv == HOSTFS_DRIVE;

        /*
         * No drive letter, so the call would fall to the current drive
         * -- but the character devices have no drive at all. "CON:",
         * "AUX:" and "PRN:" are answered by osif()'s special-name table
         * (bdosmain.c), and this hook sits ABOVE that lookup, so
         * claiming them while C: happened to be current would take the
         * console away from every program that opens it by name and
         * hand back EPTHNF instead. The same table decides here, rather
         * than a copy of it that could drift.
         */
        if (fn == 0x3D && specname_handle(name))
            return 0;

        return run->p_curdrv == HOSTFS_DRIVE;
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

static char fill_wire[HOSTFS_LINK_MAX_PATH];

/*
 * Fill the caller's DTA from one snapshot entry.
 *
 * The entry is STATted against dir_wire, the directory the snapshot was
 * taken from, rather than by re-resolving the path: nothing is held
 * open between BDOS calls, so an Fopen in the middle of a scan -- the
 * desktop's own idiom -- cannot disturb it. The snapshot itself
 * (dir_names, dir_pos, dir_pattern) is not touched here.
 *
 * "." and ".." are the two entries dir_snapshot() invented, and they
 * are the reason for the special cases: the wire refuses both outright,
 * so the DIRECTORY ITSELF and its PARENT are what actually get STATted.
 * The name written into the DTA is still the dot name the caller
 * expects to see, which is what makes a GEM folder window offer a way
 * back up.
 */
static LONG hostfs_fill_dta(const char *host)
{
    DTA *dta = (DTA *)run->p_xdta;
    ULONG size, mtime;
    UWORD is_dir, writable;
    char clipped[13];

    strcpy(fill_wire, dir_wire);

    if (!strcmp(host, "."))
    {
        /* fill_wire already names this directory */
    }
    else if (!strcmp(host, ".."))
    {
        wire_pop(fill_wire);
    }
    else if (!wire_push(fill_wire, host))
    {
        return EFILNF;
    }

    if (hostfs_link_stat(fill_wire, &size, &is_dir, &mtime, &writable) < 0)
        return EFILNF;

    hostfs_host_to_atari(host, clipped);
    strcpy(dta->d_fname, clipped);

    /*
     * A subdirectory reports length 0, as a FAT directory entry does:
     * the host's own size for a directory is a filesystem-internal
     * number that means nothing to GEM, and it changes when files are
     * added to the folder. The helper already reports 0 for one; this
     * repeats the rule rather than trusting the wire for it.
     */
    dta->d_length = is_dir ? 0L : (LONG)size;
    dta->d_attrib = hostfs_attr(is_dir, writable);

    /*
     * The timestamp is presented in UTC, and this is a KNOWN and
     * deliberate divergence from the reference implementation, which
     * passes st_mtime through localtime() and so reports the host's
     * wall-clock time (see doc/hatari-gemdos-reference.md).
     *
     * Matching it would need the host's UTC offset, and there is no
     * clean way to learn it here: the wire carries no timezone, the machine's
     * RTC is UTC, and nothing in the guest holds a timezone database.
     * The choices were therefore a rule that is simply wrong for part
     * of the year, or a plainly documented constant offset of zero.
     * Zero is chosen: it is consistent, it is what the same file will
     * report on any host, and it can be corrected exactly once if a
     * channel for the offset ever appears -- whereas a guessed offset
     * silently produces a different answer on every machine.
     */
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
        LONG rc;

        /*
         * Claim the DTA before anything can fail.
         *
         * From here on every Fsnext through this DTA comes to us,
         * including the ones that follow a FAILED Fsfirst -- which is
         * the point. The alternative, stamping only on success, leaves
         * a DTA carrying application garbage in dt_offset_drive for
         * stock xsnext() to index drvtbl[] with.
         *
         * The scan state is emptied with it, so a failure below cannot
         * leave the PREVIOUS scan's entries reachable: without this,
         * Fsfirst("C:\NOSUCH\*.*") followed by Fsnext() answers with
         * leftovers from an unrelated directory instead of ENMFIL.
         */
        ((DTAINFO *)run->p_xdta)->dt_offset_drive = HOSTFS_DTA_MAGIC;
        dir_count = 0;
        dir_pos = 0;

        /*
         * The same resolver every other path-taking call uses: the
         * leading components name the directory to scan, the final one
         * is the match pattern. Routing Fsfirst through it is what
         * makes a folder the desktop can open also a folder whose
         * files it can open.
         */
        rc = hostfs_locate(spec, 0);
        if (rc < 0)
            return rc;

        /*
         * No 8.3 name is this long, so such a pattern can match
         * nothing; saying so here is also what keeps the copy below
         * inside dir_pattern.
         */
        if (strlen(path_leaf) >= sizeof(dir_pattern))
            return ENMFIL;
        strcpy(dir_pattern, path_leaf);

        /*
         * Remember which directory this scan covers, both so Fsnext can
         * reach it again and so hostfs_match() knows whether to keep
         * the "." and ".." entries: the drive root has no parent to
         * offer, but a subdirectory does and GEM expects to see it.
         */
        strcpy(dir_wire, path_wire);
        dir_subdir = strcmp(path_canon, "\\") ? 1 : 0;

        rc = dir_snapshot(dir_wire);
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
        if (!hostfs_match(dir_pattern, host, dir_subdir))
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
