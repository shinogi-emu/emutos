/*
 * virtio_9p.h - 9P2000.L transport over virtio-mmio
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_9P_H
#define VIRTIO_9P_H

#include "portab.h"

#define P9_MSIZE     8192       /* negotiated message size */
#define P9_ROOT_FID  0

/* 9P2000.L message types used here. */
#define P9_RLERROR    7
#define P9_TVERSION 100
#define P9_RVERSION 101
#define P9_TATTACH  104
#define P9_RATTACH  105
#define P9_TWALK    110
#define P9_RWALK    111
#define P9_TGETATTR  24
#define P9_RGETATTR  25
#define P9_TCLUNK   120
#define P9_RCLUNK   121
#define P9_TLOPEN    12
#define P9_RLOPEN    13
#define P9_TREADDIR  40
#define P9_RREADDIR  41
#define P9_TREAD    116
#define P9_RREAD    117

#define P9_GETATTR_BASIC 0x000007ffUL

#define P9_O_RDONLY  0UL

void virtio_9p_attach(WORD slot);
WORD virtio_9p_present(void);
LONG p9_attach(void);
LONG p9_errno_to_gemdos(ULONG err);
LONG p9_walk(ULONG fid, ULONG newfid, const char *name);
LONG p9_getattr(ULONG fid, ULONG *size, UWORD *is_dir, ULONG *mtime,
                UWORD *writable);
void p9_clunk(ULONG fid);
LONG p9_lopen(ULONG fid, ULONG flags);
LONG p9_readdir(ULONG fid, ULONG offset, UBYTE *buf, ULONG buflen);
LONG p9_read(ULONG fid, ULONG offset, UBYTE *buf, ULONG count);
ULONG p9_dirent_next(const UBYTE *buf, ULONG buflen, ULONG off, char *name,
                     ULONG namemax, ULONG *next_off);

#endif /* VIRTIO_9P_H */
