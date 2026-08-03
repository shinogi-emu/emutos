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

void hostfs_init(void);
WORD hostfs_claims(WORD fn, short *pw);
LONG hostfs_dispatch(WORD fn, short *pw);

#endif /* HOSTFS_H */
