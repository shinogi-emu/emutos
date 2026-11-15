/*
 * vdiscreen.h - the numbers screen() in vdi_asm.S shares with the C code
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Included by assembler and C.  Only plain numbers, so that it can be:
 * vdi_main.c checks each of them against the structures with
 * _Static_assert(), so a change to Vwk or to a table stops the build
 * instead of breaking the VDI.  The assembler screen() exists when
 * CONF_WITH_VDI_FASTDRAW is set, not on ColdFire.
 */

#ifndef VDISCREEN_H
#define VDISCREEN_H

/* TRUECOLOR_MODE is more than this many planes */
#define VS_MAX_INDEXED_PLANES   8

/* the opcodes: the closing calls, and the last of the two main tables */
#define VS_OP_CLSWK     2               /* V_CLSWK_OP: 1 is v_opnwk */
#define VS_OP_CLSVWK    101             /* V_CLSVWK_OP: 100 is v_opnvwk */
#define VS_OP_TAB1_LAST 39
#if CONF_WITH_EXTENDED_MOUSE
# define VS_OP_TAB2_LAST 134
#else
# define VS_OP_TAB2_LAST 131
#endif

/* rows of the two tables (struct vdi_jmptab): two words, then the function */
#define VS_ROW_SHIFT    3               /* 8 bytes */
#define VS_ROW_NPTSOUT  0
#define VS_ROW_NINTOUT  2
#define VS_ROW_OP       4

/* the words of CONTRL used */
#define VS_CONTRL_OP        0           /* CONTRL[0] */
#define VS_CONTRL_NPTSOUT   4           /* CONTRL[2] */
#define VS_CONTRL_NINTOUT   8           /* CONTRL[4] */
#define VS_CONTRL_HANDLE    12          /* CONTRL[6] */

/* fill_style of a user pattern (FIS_USER) */
#define VS_FIS_USER     4

/* a pointer of vwk_ptr[] is four bytes */
#define VS_VWKPTR_SHIFT 2

/* offsets in struct Vwk_, a workstation */
#define VS_VWK_CLIP         2
#define VS_VWK_CUR_FONT     4
#define VS_VWK_MULTIFILL    10
#define VS_VWK_FILL_STYLE   36
#if CONF_WITH_VIDEL
# define VS_VWK_WRT_MODE    1192        /* after ud_patrn[], 32*16 words */
#elif CONF_WITH_TT_SHIFTER
# define VS_VWK_WRT_MODE    424         /* 8*16 words */
#else
# define VS_VWK_WRT_MODE    296         /* 4*16 words */
#endif
#define VS_VWK_XMN_CLIP     (VS_VWK_WRT_MODE + 4)
#define VS_VWK_XMX_CLIP     (VS_VWK_WRT_MODE + 6)
#define VS_VWK_YMN_CLIP     (VS_VWK_WRT_MODE + 8)
#define VS_VWK_YMX_CLIP     (VS_VWK_WRT_MODE + 10)
/* ext (16 bit colour) and bez_qual follow ymx_clip */
#if CONF_WITH_VDI_16BIT
# define VS_VWK_EXT_SIZE    4
#else
# define VS_VWK_EXT_SIZE    0
#endif
#if CONF_WITH_VDI_NVDI_EXT
# define VS_VWK_BEZ_SIZE    2
#else
# define VS_VWK_BEZ_SIZE    0
#endif
#define VS_VWK_OUTLINE_HEAD (VS_VWK_YMX_CLIP + 2 + VS_VWK_EXT_SIZE + VS_VWK_BEZ_SIZE)
#define VS_VWK_OUTLINE_FONT (VS_VWK_OUTLINE_HEAD + 94)  /* head 90 bytes, outline_face 4 */
#if CONF_WITH_VDI_OUTLINE_FONTS
# define VS_VWK_COL         (VS_VWK_OUTLINE_FONT + 4 + 10)  /* five words follow outline_font */
#else
# define VS_VWK_COL         VS_VWK_OUTLINE_HEAD
#endif
#define VS_VWK_BM_ADDR      (VS_VWK_COL + 134)  /* col is 134 bytes, then bm.addr */

#endif /* VDISCREEN_H */
