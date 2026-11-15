/*
 * vdicolblt.h - the block colblt (vdi_blit.S) works in
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Included by vdi_blit.S, which defines colblt, and by its callers in
 * assembler (text_cols in vdi_tblit.S): the offsets of the fields of the
 * block they share.  Only plain numbers.  What each field is: the comment
 * of colblt in vdi_blit.S.
 */

#ifndef VDICOLBLT_H
#define VDICOLBLT_H

#define CB_SSTEP        0
#define CB_DSTEP        4
#define CB_LM           8       /* left column: mask, then its complement */
#define CB_NLM          12
#define CB_RM           16      /* right column */
#define CB_NRM          20
#define CB_ROWS         24
#define CB_MID          26      /* whole columns between the two ends, -1 */
#define CB_PAIRS        28
#define CB_SHIFT        30      /* left shift of the two-word window (0-15) */
#define CB_KNEG         32      /* -1: the first window starts before the source */
#define CB_LAST         34      /* 1: the last column has no source word to add */
#define CB_COLS         36      /* destination columns - 1 */
#define CB_MODE         38      /* 0, or the text writing mode 1-4 */
#define CB_OPS          40
#define CB_K            48      /* per pair: ka, kx, sa, sx as long words */
#define CB_F            112     /* per pair: the foreground bits as masks */
#define CB_SIZE         128

#endif /* VDICOLBLT_H */
