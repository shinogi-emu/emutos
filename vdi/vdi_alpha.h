/*
 * vdi_alpha.h - images with alpha: blending into 16-bit pixels
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The arithmetic of blending 32-bit pixels with straight alpha into the
 * 16-bit pixels of the VDI (CONF_WITH_VDI_ALPHA; vr_transfer_bits() with
 * mode T_BLEND).  The functions touch nothing but their arguments, so that
 * they can be checked on the host.  The pixels are rrrrrggggggbbbbb, a word
 * with the high byte first in memory.
 */

#ifndef VDI_ALPHA_H
#define VDI_ALPHA_H

#include "portab.h"

/* Wc' of a full weight (0xffff) */
#define VDI_ALPHA_FULL  65536UL

/* the weight w of 0-65535 as Wc' of 0-65536: w + (w >> 15) */
ULONG vdi_alpha_weight(UWORD w);

/* kc of 0-256 of the source alpha 0-255 under the weight Wc' */
UWORD vdi_alpha_k(UWORD alpha, ULONG wprime);

/* the pixel d with the colour 0x00rrggbb mixed in by kr, kg, kb (each 0-256) */
UWORD vdi_alpha_mix565(UWORD d, ULONG rgb, UWORD kr, UWORD kg, UWORD kb);

/*
 * count pixels of 4 bytes at src (alpha first: PX_ARGB32, else alpha last:
 * PX_RGBA32) blended 1:1 into the 565 words at dst, under the weights wp[3]
 * (Wc' of red, green, blue); pixels with an alpha of 0 are not touched.  On
 * the 68k, dst and src are at even addresses.
 */
void vdi_alpha_row565(UBYTE *dst, const UBYTE *src, LONG count, BOOL alpha_first, const ULONG wp[3]);

#endif /* VDI_ALPHA_H */
