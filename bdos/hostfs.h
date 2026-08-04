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
 * READDIR returns ONE entry and a cookie per call, so a directory scan
 * is a loop of requests. A helper whose cookie advanced but whose
 * enumeration never ended would make that loop run forever, so the
 * number of steps is bounded as well as the number of entries stored.
 * This is far more than HOSTFS_MAX_ENTRIES needs -- the scan keeps
 * reading past the table so it can report the true shortfall -- and is
 * reached only by a helper that is broken or hostile.
 */
#define HOSTFS_MAX_DIRSTEPS 4096

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
 * There is no fid table any more.
 *
 * The 9P transport bound a fid to every slot (fid = base + slot) so a
 * missed error path could not leak one. The virtio-serial helper hands
 * out its own handles instead, and it forgets all of them on HELLO --
 * which the guest sends once at boot -- so a leak cannot outlive a
 * reboot and cannot be made unrepresentable here either. What replaces
 * the old invariant is stated on hostfs_files[] in hostfs.c: a slot is
 * marked used only while it holds a live host handle.
 */

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

/*
 * The internal face of the module.
 *
 * hostfs_claims()/hostfs_dispatch() see only what arrives through the
 * GEMDOS trap, and program loading does not arrive that way: xexec()
 * (bdos/proc.c) and kpgmld() (bdos/kpgmld.c) call xopen(), xread(),
 * xlseek() and xclose() directly, so a .PRG on C: used to be looked for
 * in the FAT directory tree and reported missing. Those internal entry
 * points route here instead -- by PATH for the open, by HANDLE for the
 * rest -- which is why the loader itself needs no change: it still
 * decides how a program is loaded, and only where the bytes come from
 * is answered here.
 *
 * Routing by handle RANGE rather than by "is this slot in use" is the
 * same rule hostfs_claims() follows for the handle-bearing calls, and
 * for the same reason: a handle of 0x0100 or above must never be the
 * one stock code looks up. hostfs_in_range() is exported so that rule
 * can be stated identically at both ends rather than copied.
 */
WORD hostfs_owns_path(const char *path);
WORD hostfs_in_range(WORD h);
LONG hostfs_open(const char *path, WORD mode);
LONG hostfs_close(WORD h);
LONG hostfs_read(WORD h, ULONG count, UBYTE *buf);
LONG hostfs_write(WORD h, ULONG count, const UBYTE *buf);
LONG hostfs_seek(LONG offset, WORD h, WORD mode);

#endif /* HOSTFS_H */
