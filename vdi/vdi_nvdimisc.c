/*
 * vdi_nvdimisc.c - vr_clip_rects (171)
 *
 * vr_clip_rects_by_dst (sub-function 0) and vr_clip_rects_by_src (1) and
 * their forms with 32-bit coordinates (2 and 3): clip one of a source and a
 * destination rectangle to a clipping rectangle and bring the other along,
 * in proportion where the two differ in size.  The arithmetic is in
 * vdi_nvdimath.c.
 *
 * ptsin: the clipping, source and destination rectangles (x1, y1, x2, y2,
 * both ends inclusive), as WORDs (6 points) or as LONGs (12 points).
 * ptsout: the clipped source and destination, 4 or 8 points; intout[0] is
 * 1, or 0 when nothing is left (the pair then comes back as it went in) or
 * when the call is not one this knows (no points out then).
 *
 * v_bez_qual (escape 5, sub-function 99) is in vdi_bezier.c.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "vdi_nvdimath.h"

#if CONF_WITH_VDI_NVDI_EXT

/* a longword out of two words: they need not be aligned for one */
static LONG get_long(const WORD *p)
{
    return ((LONG)p[0] << 16) | (LONG)(UWORD)p[1];
}


static void put_long(WORD *p, LONG v)
{
    p[0] = (WORD)(v >> 16);
    p[1] = (WORD)v;
}


/* the rectangle at 'p', 'wide' for LONG coordinates */
static void get_rect(const WORD *p, WORD wide, VDI_RECT32 *r)
{
    if (wide)
    {
        r->x1 = get_long(p);
        r->y1 = get_long(p + 2);
        r->x2 = get_long(p + 4);
        r->y2 = get_long(p + 6);
    }
    else
    {
        r->x1 = p[0];
        r->y1 = p[1];
        r->x2 = p[2];
        r->y2 = p[3];
    }
}


static void put_rect(WORD *p, WORD wide, const VDI_RECT32 *r)
{
    if (wide)
    {
        put_long(p, r->x1);
        put_long(p + 2, r->y1);
        put_long(p + 4, r->x2);
        put_long(p + 6, r->y2);
    }
    else
    {
        p[0] = (WORD)r->x1;
        p[1] = (WORD)r->y1;
        p[2] = (WORD)r->x2;
        p[3] = (WORD)r->y2;
    }
}


void vdi_vr_clip_rects(Vwk *vwk)
{
    WORD sub = CONTRL[5];
    WORD wide, by_src, ok, step;
    VDI_RECT32 r[3], src, dst;

    (void)vwk;
    INTOUT[0] = 0;
    CONTRL[4] = 1;
    wide = sub >= 2;
    /* a short call is as bad as an unknown sub-function: nothing is read */
    if (sub < 0 || sub > 3 || CONTRL[1] < (wide ? 12 : 6))
    {
        CONTRL[2] = 0;
        return;
    }
    by_src = sub & 1;
    step = wide ? 8 : 4;

    /* r[0] is the clip, r[1] the source and r[2] the destination */
    get_rect(PTSIN, wide, &r[0]);
    get_rect(PTSIN + step, wide, &r[1]);
    get_rect(PTSIN + 2 * step, wide, &r[2]);

    if (by_src)
        ok = clip_pair(&r[0], &r[1], &r[2], &src, &dst);
    else
        ok = clip_pair(&r[0], &r[2], &r[1], &dst, &src);
    if (!ok)
    {
        src = r[1];
        dst = r[2];
    }

    put_rect(PTSOUT, wide, &src);
    put_rect(PTSOUT + step, wide, &dst);
    CONTRL[2] = wide ? 8 : 4;
    INTOUT[0] = ok;
}

#endif /* CONF_WITH_VDI_NVDI_EXT */
