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

/*
 * a * b / c, rounded down, with a 64-bit product: for a >= 0, b > 0, c > 0.
 * A quotient over 0x7fffffff gives 0x7fffffff; an operand outside those
 * limits gives 0.
 */
LONG muldiv32(LONG a, LONG b, LONG c);

/* a rectangle with 32-bit corners, both ends inclusive */
typedef struct
{
    LONG x1;
    LONG y1;
    LONG x2;
    LONG y2;
} VDI_RECT32;

/*
 * vr_clip_rects: clip the side 'clipped_side' of a pair of rectangles to
 * 'clip' and map the clipped part onto 'other', in proportion where the
 * two differ in size (the discrete, inclusive form of fVDI's bring_along:
 * m1 = b1 + (n1 - a1) * sb / sa, m2 = b1 + ((n2 - a1 + 1) * sb - 1) / sa).
 * Returns 1 and the two results, or 0, and nothing is set, when the
 * clipped part is empty or when a rectangle is upside down or spans more
 * than 0x7fffffff.
 */
WORD clip_pair(const VDI_RECT32 *clip, const VDI_RECT32 *clipped_side,
               const VDI_RECT32 *other, VDI_RECT32 *out_clipped,
               VDI_RECT32 *out_other);

#endif /* VDI_NVDIMATH_H */
