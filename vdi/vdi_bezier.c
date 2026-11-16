/*
 * vdi_bezier.c - Bezier curves: v_bez(), v_bez_fill(), v_bez_on()/v_bez_off()
 * and v_bez_qual()
 *
 * The curves are made as fVDI makes them (engine/bezier.c): each section is
 * divided in two until it is small enough, to a depth that depends on its
 * size and on the quality asked for (a depth scale, 0 the finest, 9 the
 * coarsest), less finely where it lies outside the clipping rectangle.  The
 * points then go to the ordinary line and fill code: a move point starts a
 * new polyline, or a new contour of the filled area.  The contours are filled
 * at once, even-odd, as the outlines of one polygon: with v_fillarea()'s rule
 * for the rows (the top row of the whole area is left out, as v_fillarea()
 * leaves out a polygon's) and for the ends of the edges.
 *
 * The arithmetic of the curves (bez_depth(), do_bez4(), the point counting
 * and copying of calc_bez()) is fVDI's, in vdi_bezcore.c.  Differences:
 * - a curve must have its four points: fVDI also takes one that starts on
 *   the third last point, and reads a point beyond the array for it;
 * - the flags of the points are read only as far as CONTRL[3] reaches, and
 *   only once (copied: the counting and the making use the copy), CONTRL[1]
 *   and CONTRL[3] once, and the making is bounded by the buffer, not only
 *   by the count made before it;
 * - intout[1] is the number of move points (fVDI answers 1 when anything was
 *   made of curves or moves); the extent covers exactly the points made;
 * - each polyline is drawn as v_pline() draws one (width, ends), and a
 *   filled area as v_fillarea() fills (fVDI's own fill also fills the top
 *   row), with its perimeter when vsf_perimeter() asks for one;
 * - at most MAX_VERTICES points are made (fVDI: 1024), in a buffer of the
 *   VDI's: the drawing makes no OS call, so it cannot be needed twice at once.
 *
 * Copyright 1999-2001/2003, Johan Klockars (fVDI)
 * Copyright 1999, Caldera Thin Clients, Inc.
 * Copyright (C) 2004-2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The method, from fVDI's bezier.c:
 *
 *   Description       : Fast Bezier approximation using
 *                       four control points.
 *
 *   Original Author   : William D. Herndon, CCP Software, 29.04.88
 *                       See also the best book so far on beziers:
 *                        Algorithms for graphics & image processing,
 *                        Theo Pavlidis, Bell Labs,
 *                        Computer Science Press ISBN 0-914894-65-X.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "intmath.h"
#include "lineavars.h"
#include "string.h"
#include "vdi_bezcore.h"

#if HAVE_BEZIER

#define MAX_POINTS      MAX_VERTICES

/*
 * the flags of the points, copied once from intin; the points made, and a
 * bit for each that starts a new polyline or contour
 */
static UBYTE bez_flagcopy[MAX_POINTS];
static Point bez_pts[MAX_POINTS + 1];
static UBYTE bez_break[(MAX_POINTS + 8) / 8];

#define IS_BREAK(i)     (bez_break[(i) >> 3] & (0x80 >> ((i) & 7)))


/*
 * Copy the flags of the points out of intin, once: each point's is a byte,
 * in the byte order of the PC (point 0 is the low byte of intin[0]); there
 * are those of the points before the last, as far as intin reaches.
 * Returns how many.
 */
static WORD copy_flags(WORD count, WORD nintin)
{
    LONG n = (LONG)count - 1, i;
    UWORD w;

    if (n > 2L * nintin)
        n = 2L * nintin;
    if (n > MAX_POINTS)
        n = MAX_POINTS;
    for (i = 0; i + 1 < n; i += 2)
    {
        w = (UWORD)INTIN[i >> 1];
        bez_flagcopy[i] = (UBYTE)w;
        bez_flagcopy[i + 1] = (UBYTE)(w >> 8);
    }
    if (i < n)
        bez_flagcopy[i] = (UBYTE)INTIN[i >> 1];
    return (n > 0) ? (WORD)n : 0;
}


/* the number of points, read once (the entry has limited it to MAX_VERTICES; so is it here) */
static WORD latch_count(void)
{
    WORD count = CONTRL[1];

    if (count < 0)
        count = 0;
    if (count > MAX_POINTS)
        count = MAX_POINTS;
    return count;
}


/* the clipping rectangle (x1, y1, x2, y2) the division looks at */
static void bez_clip(const Vwk *vwk, WORD *clip)
{
    if (vwk->clip)
    {
        clip[0] = vwk->xmn_clip;
        clip[1] = vwk->ymn_clip;
        clip[2] = vwk->xmx_clip;
        clip[3] = vwk->ymx_clip;
    }
    else
    {
        clip[0] = clip[1] = 0;
        clip[2] = xres;
        clip[3] = yres;
    }
}


/*
 * Make the points of 'count' points (PTSIN) with 'nflags' flags copied, at
 * the workstation's quality, or coarser where they would be too many; -1 if
 * they cannot be made (or there is nothing to make: *used FALSE)
 */
static WORD make_points(Vwk *vwk, WORD count, WORD nflags, BOOL close, WORD *moves, BOOL *used)
{
    const BezPoint *pts = (const BezPoint *)PTSIN;
    WORD clip[4], scale, n = -1;

    bez_clip(vwk, clip);
    for (scale = vwk->bez_qual; scale <= BEZ_COARSEST; scale++)
    {
        if (bez_count(pts, count, bez_flagcopy, nflags, scale, close, moves, used) > MAX_POINTS)
            continue;
        if (!*used)
            break;
        n = bez_generate(pts, count, bez_flagcopy, nflags, scale, close, clip,
                         (BezPoint *)bez_pts, bez_break, MAX_POINTS);
        if (n >= 0)
            break;
    }
    return *used ? n : -1;
}


/* intout[0] the points, [1] the moves, [2-5] 0; ptsout the extent of the points */
static void bez_answer(const Point *p, WORD n, WORD moves)
{
    WORD i, xmin, ymin, xmax, ymax;

    xmin = xmax = ymin = ymax = 0;
    if (n > 0)
    {
        xmin = xmax = p->x;
        ymin = ymax = p->y;
        for (i = 1; i < n; i++)
        {
            p++;
            if (p->x < xmin)
                xmin = p->x;
            else if (p->x > xmax)
                xmax = p->x;
            if (p->y < ymin)
                ymin = p->y;
            else if (p->y > ymax)
                ymax = p->y;
        }
    }
    PTSOUT[0] = xmin;
    PTSOUT[1] = ymin;
    PTSOUT[2] = xmax;
    PTSOUT[3] = ymax;
    INTOUT[0] = n;
    INTOUT[1] = moves;
    INTOUT[2] = INTOUT[3] = INTOUT[4] = INTOUT[5] = 0;
    CONTRL[2] = 2;
    CONTRL[4] = 6;
}


/* a polyline as v_pline() draws one */
static void bez_polyline(Vwk *vwk, Point *p, WORD n)
{
    if (vwk->line_width == 1)
    {
        polyline(vwk, p, n, vwk->line_color);
        if ((vwk->line_beg | vwk->line_end) & ARROWED)
            arrow(vwk, p, n);
    }
    else
        wideline(vwk, p, n);
}


/*
 * v_bez - v_pline() with sub-opcode 13 (or after v_bez_on()) and intin
 *
 * ptsin the points; intin their flags, a byte each in the byte order of the
 * PC: bit 0, the point and the next three are a curve (start, two direction
 * points, end); bit 1, the point starts a new polyline.
 */
void v_bez(Vwk *vwk)
{
    WORD count = latch_count(), nflags = copy_flags(count, CONTRL[3]), n, moves, i, start;
    BOOL used;

    n = make_points(vwk, count, nflags, FALSE, &moves, &used);
    if (n < 0)
    {
        /* nothing to make of it: a polyline, as fVDI; too many points: nothing */
        if (!used)
        {
            bez_polyline(vwk, (Point *)PTSIN, count);
            moves = 0;
        }
        bez_answer((const Point *)PTSIN, count, moves);
        return;
    }

    for (i = 1, start = 0; i <= n; i++)
    {
        if (i == n || IS_BREAK(i))
        {
            if (i - start >= 2)
                bez_polyline(vwk, bez_pts + start, i - start);
            start = i;
        }
    }
    bez_answer(bez_pts, n, moves);
}


/* sort the x of the intersections */
static void sort_words(WORD *buf, WORD count)
{
    WORD i, j, v;

    for (i = 1; i < count; i++)
    {
        v = buf[i];
        for (j = i; j > 0 && buf[j - 1] > v; j--)
            buf[j] = buf[j - 1];
        buf[j] = v;
    }
}


/*
 * fill the contours of bez_pts (each closed, separated by the breaks) even-odd,
 * as clc_flit() fills a polygon
 */
static void bez_fill_contours(Vwk *vwk, WORD n)
{
    const VwkClip *clipper = VDI_CLIP(vwk);
    WORD maxy, miny, y, i, intersections;
    WORD *buf;
    VwkAttrib attr;
    Rect rect;

    maxy = miny = bez_pts[0].y;
    for (i = 1; i < n; i++)
    {
        if (bez_pts[i].y < miny)
            miny = bez_pts[i].y;
        else if (bez_pts[i].y > maxy)
            maxy = bez_pts[i].y;
    }
    if (vwk->clip)
    {
        if (maxy < clipper->ymn_clip || miny > clipper->ymx_clip)
            return;
        if (miny < clipper->ymn_clip)
            miny = clipper->ymn_clip - 1;
        if (maxy > clipper->ymx_clip)
            maxy = clipper->ymx_clip;
    }
    Vwk2Attrib(vwk, &attr, vwk->fill_color);

    for (y = maxy; y > miny; y--)
    {
        intersections = 0;
        buf = vdishare.main.fill_buffer;
        for (i = 0; i < n - 1; i++)
        {
            WORD y1, y2, dy, dy1, dy2, x1, x2, dx;

            if (IS_BREAK(i + 1))
                continue;               /* from the end of one contour to the next */
            y1 = bez_pts[i].y;
            y2 = bez_pts[i + 1].y;
            dy = y2 - y1;
            if (!dy)
                continue;
            dy1 = y - y1;
            dy2 = y - y2;
            if ((dy1 ^ dy2) >= 0)
                continue;
            if (intersections >= MAX_VERTICES)
                break;
            x1 = bez_pts[i].x;
            x2 = bez_pts[i + 1].x;
            dx = (x2 - x1) << 1;
            intersections++;
            if (dx < 0)
                *buf++ = ((mul_div(dy2, dx, dy) + 1) >> 1) + x2;
            else
                *buf++ = ((mul_div(dy1, dx, dy) + 1) >> 1) + x1;
        }
        if (intersections < 2)
            continue;
        sort_words(vdishare.main.fill_buffer, intersections);
        buf = vdishare.main.fill_buffer;
        for (i = intersections / 2; i > 0; i--)
        {
            WORD x1 = *buf++, x2 = *buf++;

            if (attr.clip)
            {
                if (x1 < clipper->xmn_clip)
                {
                    if (x2 < clipper->xmn_clip)
                        continue;
                    x1 = clipper->xmn_clip;
                }
                if (x2 > clipper->xmx_clip)
                {
                    if (x1 > clipper->xmx_clip)
                        continue;
                    x2 = clipper->xmx_clip;
                }
            }
            rect.x1 = x1;
            rect.y1 = y;
            rect.x2 = x2;
            rect.y2 = y;
            draw_rect_common(&attr, &rect);
        }
    }
}


/*
 * v_bez_fill - v_fillarea() with sub-opcode 13 (or after v_bez_on()) and
 * intin: the curves and lines as v_bez(), each polyline closed, filled
 */
void v_bez_fill(Vwk *vwk)
{
    WORD count = latch_count(), nflags = copy_flags(count, CONTRL[3]), n, moves, i, start;
    BOOL used;

    n = make_points(vwk, count, nflags, TRUE, &moves, &used);
    if (n < 0)
    {
        if (!used)
        {
            polygon(vwk, (Point *)PTSIN, count);
            moves = 0;
        }
        bez_answer((const Point *)PTSIN, count, moves);
        return;
    }

    if (n >= 2)
        bez_fill_contours(vwk, n);
    if (vwk->fill_per == TRUE)
    {
        LN_MASK = 0xffff;
        for (i = 1, start = 0; i <= n; i++)
        {
            if (i == n || IS_BREAK(i))
            {
                if (i - start >= 2)
                    polyline(vwk, bez_pts + start, i - start, vwk->fill_color);
                start = i;
            }
        }
    }
    bez_answer(bez_pts, n, moves);
}


/*
 * v_bez_control - v_gdp() 13: v_bez_on() (ptsin count not 0) and v_bez_off()
 *
 * v_bez_on: from now on v_pline() and v_fillarea() with intin draw Beziers,
 * at the finest quality; intout[0] is the greatest depth of division (7).
 * v_bez_off: no longer; nothing is returned.  As fVDI.
 */
void v_bez_control(Vwk *vwk)
{
    if (CONTRL[1] != 0)
    {
        vwk->bez_on = 1;
        vwk->bez_qual = BEZ_FINEST;
        INTOUT[0] = BEZ_MAX_DEPTH;
        CONTRL[4] = 1;
    }
    else
        vwk->bez_on = 0;
}

#endif /* HAVE_BEZIER */


#if WITH_BEZ_QUAL
/*
 * v_bez_qual - the quality of the curves (escape 5, sub-function 99,
 * intin[0] = 32, intin[2] the percentage, 100 the finest)
 *
 * As fVDI: the percentage is mapped onto the depth scale, 0 the finest and
 * 9 the coarsest, and intout[0] is the quality the scale really gives.
 * Kept also when Beziers are not drawn (the NVDI calls report it).
 */
void v_bez_qual(Vwk *vwk)
{
    WORD percent = INTIN[2], scale;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    scale = (WORD)(((100L - percent) * 9 + 50) / 100);
    vwk->bez_qual = scale;
    INTOUT[0] = (WORD)(100 - ((LONG)scale * 100 + 4) / 9);
    CONTRL[4] = 1;
}
#endif /* WITH_BEZ_QUAL */
