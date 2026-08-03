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

void virtio_9p_attach(WORD slot);
WORD virtio_9p_present(void);
LONG p9_attach(void);
LONG p9_errno_to_gemdos(ULONG err);

#endif /* VIRTIO_9P_H */
