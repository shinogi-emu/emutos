/*
 * vdi_nvdimath.h - the arithmetic of the NVDI colour calls
 *
 * Pure functions: no hardware, no globals.  The host tests build
 * vdi_nvdimath.c on its own.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_NVDIMATH_H
#define VDI_NVDIMATH_H

#include "portab.h"

/* a VDI colour component, 0-1000, as a 16-bit one, 0-65535 (out of range: clamped) */
UWORD vdi_to_rgb16(WORD value);

/* a 16-bit component as a VDI one: the nearest thousandth */
WORD rgb16_to_vdi(UWORD value);

/*
 * the Falcon 16-bit pixel rrrrrggggg0bbbbb from 16-bit components: the
 * top five bits of each, cut and not rounded
 */
UWORD vdi_pack16(UWORD red, UWORD green, UWORD blue);

/* and back: each five-bit value widened to 16 bits by repeating its top bits */
void vdi_unpack16(UWORD pixel, UWORD *red, UWORD *green, UWORD *blue);

/*
 * the squared distance of two colours of 0-1000 components (out of range
 * components are clamped first)
 */
LONG vdi_color_dist(const WORD *a, const WORD *b);

/*
 * the entry of a table of 'count' colours (0-1000 components) that is
 * nearer to 'rgb' than *best, which is then set to its distance.  Returns
 * its index, or -1 if there is none; of equal distances the lowest index
 * wins.  Start with *best = VDI_DIST_FAR; to scan several tables in turn,
 * leave *best as it comes back.
 */
#define VDI_DIST_FAR    0x7fffffffL
WORD vdi_nearest_index(const WORD (*table)[3], WORD count, const WORD *rgb, LONG *best);

#endif /* VDI_NVDIMATH_H */
