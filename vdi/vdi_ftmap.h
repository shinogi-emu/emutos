/*
 * vdi_ftmap.h - character set tables of the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * Only used by the 32-bit int side (vdi_ft2.c and vdi_ftmap.c).
 *
 * In the Atari character set, a code is mapped to Unicode in two steps:
 * Atari2Bics gives its index in the Bitstream International Character
 * Set, which Bics2Unicode turns into Unicode (0xffff: none).  An
 * SPDCHAR.MAP file can replace the first step.
 */

#ifndef VDI_FTMAP_H
#define VDI_FTMAP_H

#include "portab.h"

#define BICS_COUNT 564

extern const WORD Atari2Bics[256];
extern const UWORD Bics2Unicode[BICS_COUNT];

#endif /* VDI_FTMAP_H */
