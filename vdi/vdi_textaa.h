/*
 * vdi_textaa.h - anti-aliased text: coverage blended into 16-bit pixels
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The arithmetic of anti-aliased outline text (CONF_WITH_VDI_TEXT_AA): the
 * outline font engine renders a string as 8-bit coverage (see vdi_ft2.h),
 * vdi_outline.c turns it for rotated text and clips it, and the functions
 * below blend the text colour into the pixels by it.  They touch nothing
 * but their arguments, so that they can be checked on the host.
 *
 * The pixels are those of the 16-bit VDI: RRRRRGGGGG0BBBBB.
 */

#ifndef VDI_TEXTAA_H
#define VDI_TEXTAA_H

#include "portab.h"

/* coverage is blended in this many steps: 0 leaves a pixel, 32 is the text colour */
#define VDI_AA_LEVELS   32

/* how vdi_aa_blend16() blends */
#define VDI_AA_TRANS    0       /* the text colour over the pixels there */
#define VDI_AA_REPLACE  1       /* the text colour over the background colour, everywhere */

/* the step of a coverage of 0 to 255 */
#define VDI_AA_LEVEL(c) ((UWORD)(((UWORD)(c) + 4) >> 3))

/*
 * the colour level / VDI_AA_LEVELS of the way from pixel d to pixel f,
 * each component rounded to the nearest; bit 5 is left 0
 */
UWORD vdi_aa_mix16(UWORD d, UWORD f, UWORD level);

/*
 * blend fg by the coverage cov (cov_bytes a row) into the width x height
 * pixels at dst (dst_bytes a row): over the pixels there (VDI_AA_TRANS),
 * or over bg in every pixel (VDI_AA_REPLACE)
 */
void vdi_aa_blend16(UWORD *dst, LONG dst_bytes, const UBYTE *cov, LONG cov_bytes,
                    LONG width, LONG height, UWORD fg, UWORD bg, WORD mode);

/*
 * turn the w x h coverage image src (src_bytes a row) counterclockwise by
 * angle, 900, 1800 or 2700 tenths of a degree, into dst (dst_bytes a row),
 * h x w, w x h or h x w: as vdi_outline.c turns one-plane text
 */
void vdi_aa_turn(const UBYTE *src, LONG src_bytes, LONG w, LONG h, WORD angle,
                 UBYTE *dst, LONG dst_bytes);

#endif /* VDI_TEXTAA_H */
