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

#define HOSTFS_DRIVE 2          /* C: */

#define HOSTFS_MAX_ENTRIES 128
#define HOSTFS_MAX_NAME    64

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

void hostfs_init(void);
WORD hostfs_claims(WORD fn, short *pw);
LONG hostfs_dispatch(WORD fn, short *pw);

#endif /* HOSTFS_H */
