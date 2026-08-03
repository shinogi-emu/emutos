/*
 * hostfs.h - host folder as a GEMDOS drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef HOSTFS_H
#define HOSTFS_H

#include "portab.h"
#include "sysconf.h"    /* LEN_ZPATH bounds HOSTFS_MAX_PATH -- see below */

#define HOSTFS_DRIVE 2          /* C: */

/*
 * The Fsfirst/Fsnext snapshot is one flat table, so its size is the
 * hard limit on how many entries a single directory can show. 512 at 64
 * bytes each is 32KB of BSS, which this machine has no shortage of, and
 * it covers any host folder a person would plausibly point the drive
 * at. A directory bigger than this is reported, not silently clipped --
 * see dir_snapshot().
 */
#define HOSTFS_MAX_ENTRIES 512
#define HOSTFS_MAX_NAME    64

/*
 * Treaddir is paged: the device decides how many entries fit in one
 * reply, and the snapshot re-issues the request until a page comes back
 * empty. A device that returned entries whose offsets never advanced
 * would make that loop run forever, so the number of pages is bounded
 * as well. At a couple of dozen entries per page this is far more than
 * HOSTFS_MAX_ENTRIES needs, and is reached only by a device that is
 * broken or hostile.
 */
#define HOSTFS_MAX_DIRPAGES 256

/*
 * Open files live in a private GEMDOS handle range that EmuTOS can
 * neither allocate nor reach. Stock handles are 0..5 (standard, mapped
 * through the basepage) and 6..80 (sft[75] plus NUMSTD), so 0x0100 and
 * up cannot collide with anything EmuTOS hands out, and the hostfs hook
 * in osif() sits above every handle translation.
 *
 * Two consequences are accepted rather than fixed, because neither is
 * needed to read a file: Fforce()/Fdup() cannot reach a hostfs handle
 * (the basepage's p_uft[] is SBYTE, so 0x0100 does not fit), and Pterm
 * does not auto-close one (ixterm() walks sft, which these handles are
 * not in). A process that exits with a host file open leaks its slot
 * until the next boot.
 */
#define HOSTFS_HANDLE_BASE 0x0100
#define HOSTFS_MAX_FILES   8

/*
 * 9P fids are bound statically, fid = HOSTFS_FID_BASE + slot, rather
 * than allocated. Nothing but an explicit Tclunk frees a fid, so an
 * allocator would leak one on every missed error path and then fail all
 * at once, far from the bug. Binding the fid to the slot makes fids a
 * function of the slot table rather than of the operation count, so a
 * leak is not merely avoided but unrepresentable. Fid 0 is the root and
 * fid 1 the transient scratch used for walks and directory reads; the
 * gap up to 16 leaves room for further reserved fids.
 */
#define HOSTFS_FID_BASE    16

/*
 * Reserved scratch fids, below the per-slot range. Fid 1 is the cursor
 * for one-off walks and directory reads; fid 2 is cloned from whatever
 * fid 1 currently points at so a directory can be opened and read
 * without destroying the cursor (an Rlopen fixes the fid to a stream,
 * and the cursor still has to be walked deeper afterwards). Fid 3 is
 * the object named by the final path component, which has to be walked
 * to from the cursor while the cursor itself stays where it is.
 */
#define HOSTFS_WALK_FID    1
#define HOSTFS_SCAN_FID    2
#define HOSTFS_ITEM_FID    3

/*
 * The current directory, as an Atari path relative to the drive root:
 * "\" at the root, "\SUB\DEEP" below it. It is not a host path and
 * never leaves the drive -- ".." is applied to this string and refuses
 * to pop the root, which is what keeps a guest inside the shared folder.
 *
 * The size is dictated by the CALLER's buffer, not by this module.
 * Dgetpath writes into whatever the application passed, and every
 * EmuTOS caller sizes that by LEN_ZPATH (aes/gemshlib.c:82,87,
 * desk/deskfpd.h:73, desk/deskapp.h:130); stock xgetdir() therefore
 * bounds its output at LEN_ZPATH - 3 (fsdir.c). A path longer than that
 * is refused with EPTHNF exactly as stock refuses a too-deep FAT path,
 * which is the one behaviour a caller is already prepared for -- the
 * alternative is writing past the end of a desktop structure.
 *
 * So this is LEN_ZPATH - 3 plus the terminator, and path_push() is
 * where the refusal happens.
 */
#define HOSTFS_MAX_PATH    (LEN_ZPATH - 3 + 1)      /* 112: 111 chars + NUL */

void hostfs_init(void);
WORD hostfs_claims(WORD fn, short *pw);
LONG hostfs_dispatch(WORD fn, short *pw);

#endif /* HOSTFS_H */
