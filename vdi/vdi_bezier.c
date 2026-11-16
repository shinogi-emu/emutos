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
 * and copying of calc_bez()) is fVDI's.  Differences:
 * - a curve must have its four points: fVDI also takes one that starts on
 *   the third last point, and reads a point beyond the array for it;
 * - the flags of the points are read only as far as CONTRL[3] reaches;
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

#if HAVE_BEZIER

#define BEZ_START       0x01    /* the point and the next three are a curve */
#define BEZ_MOVE        0x02    /* the point starts a new polyline */

#define BEZ_MIN_DEPTH       2       /* a curve is never made of fewer than 3 << 1 lines */
#define BEZ_MAX_DEPTH       7
#define COARSEST_SCALE  9       /* the depth scale: 0 the finest, 9 the coarsest */
#define MAX_POINTS      MAX_VERTICES

/* the points made, and a bit for each that starts a new polyline or contour */
static Point bez_pts[MAX_POINTS + 1];
static UBYTE bez_break[(MAX_POINTS + 8) / 8];

#define SET_BREAK(i)    (bez_break[(i) >> 3] |= (UBYTE)(0x80 >> ((i) & 7)))
#define IS_BREAK(i)     (bez_break[(i) >> 3] & (0x80 >> ((i) & 7)))


/* the flags of point i: intin is in the byte order of the PC (point 0 is the low byte of intin[0]) */
static WORD bez_flags(WORD i)
{
    UWORD w = (UWORD)INTIN[i >> 1];

    return (i & 1) ? (WORD)(w >> 8) : (WORD)(w & 0xff);
}


/* the largest of the distances of the coordinates b - v1, b - v2, b - v3 */
static WORD max3(WORD b, WORD v1, WORD v2, WORD v3)
{
    WORD m = b - v1, t;

    if (m < 0)
        m = -m;
    t = b - v2;
    if (t < 0)
        t = -t;
    if (t > m)
        m = t;
    t = b - v3;
    if (t < 0)
        t = -t;
    if (t > m)
        m = t;
    return m;
}


/*
 * bez_depth - the depth of division a curve needs to look smooth, from an
 * estimate of its area (rather too much than too little)
 */
static WORD bez_depth(const Point *p, WORD depth_scale)
{
    WORD xdiff, ydiff, depth;
    LONG bez_size;

    xdiff = max3(p[0].x, p[1].x, p[2].x, p[3].x);
    ydiff = max3(p[0].y, p[1].y, p[2].y, p[3].y);

    bez_size = (LONG)xdiff * ydiff;

    if (!(bez_size >>= 4 * BEZ_MIN_DEPTH + depth_scale - 2))
        return BEZ_MIN_DEPTH;

    for (depth = BEZ_MIN_DEPTH + 1; depth < BEZ_MAX_DEPTH; depth++)
        if (!(bez_size >>= 4))
            return depth;

    return BEZ_MAX_DEPTH;
}


/*
 * One section of a curve: its end point (the start is the anchor of the
 * next node), the direction points of the end and of the start, whether it
 * is entirely inside the clipping rectangle, and how much more it is divided.
 * Coordinates are fixed point: the pixel times 2^15, with half a pixel added.
 */
struct coords
{
    LONG x;
    LONG y;
};

struct node
{
    struct coords anchor;
    struct coords in;
    struct coords out;
    WORD onscreen;
    WORD level;
};

#define BEZ_HI(v)   ((WORD)((v) >> 16))
#define FIXED(c)    ((((LONG)(c)) << 15) + 0x4000)

/* sort two pairs and widen min/max by the second */
#define MINMAX(min, max, v3, v4)    \
    do {                            \
        WORD tmp_;                  \
        if (min > max) {            \
            tmp_ = min; min = max; max = tmp_; \
        }                           \
        if (v3 > v4) {              \
            tmp_ = v3; v3 = v4; v4 = tmp_; \
        }                           \
        if (v4 > max)               \
            max = v4;               \
        if (v3 < min)               \
            min = v3;               \
    } while (0)


/*
 * do_bez4 - the points of a curve of four control points, to 'depth'
 *
 * The first point is already there and the last one is added by the caller;
 * returns the number of points written to 'out' plus one.
 */
static WORD do_bez4(const Point *in, WORD depth, Point *out, const WORD *clip)
{
    WORD count = 0;
    struct coords mid;
    struct node *current;
    struct node bez[BEZ_MAX_DEPTH + 1];

    current = bez;
    current->onscreen = FALSE;
    current->level = depth - 1;     /* the last step has a hidden half depth */
    /* back to front, so that the points come out in the right order */
    current->anchor.x = FIXED(in[3].x);
    current->anchor.y = FIXED(in[3].y);
    current->in.x = FIXED(in[2].x);
    current->in.y = FIXED(in[2].y);
    current->out.x = FIXED(in[1].x);
    current->out.y = FIXED(in[1].y);
    (current + 1)->anchor.x = FIXED(in[0].x);
    (current + 1)->anchor.y = FIXED(in[0].y);

    for (;;)
    {
        if (!current->onscreen)
        {
            WORD xmin, xmax, ymin, ymax, v3, v4;

            xmin = BEZ_HI(current->anchor.x);
            xmax = BEZ_HI(current->in.x);
            v3 = BEZ_HI(current->out.x);
            v4 = BEZ_HI((current + 1)->anchor.x);
            MINMAX(xmin, xmax, v3, v4);

            ymin = BEZ_HI(current->anchor.y);
            ymax = BEZ_HI(current->in.y);
            v3 = BEZ_HI(current->out.y);
            v4 = BEZ_HI((current + 1)->anchor.y);
            MINMAX(ymin, ymax, v3, v4);

            xmin = xmin * 2 - 16;       /* room for the thickness of the line */
            ymin = ymin * 2 - 16;
            xmax = xmax * 2 + 1 + 16;
            ymax = ymax * 2 + 1 + 16;
            if ((xmax < clip[0]) || (xmin > clip[2]) || (ymax < clip[1]) || (ymin > clip[3]))
                current->level = 0;     /* outside: no further division */
            else if ((xmin >= clip[0]) && (xmax <= clip[2]) && (ymin >= clip[1]) && (ymax <= clip[3]))
                current->onscreen = TRUE;
        }

        if (current->level <= 0)
        {
            /* the last stage: the section as three lines */
            out->x = (WORD)(((((current + 1)->anchor.x + current->in.x) >> 1) + current->out.x) >> 16);
            out->y = (WORD)(((((current + 1)->anchor.y + current->in.y) >> 1) + current->out.y) >> 16);
            out++;
            out->x = (WORD)((((current->anchor.x + current->out.x) >> 1) + current->in.x) >> 16);
            out->y = (WORD)((((current->anchor.y + current->out.y) >> 1) + current->in.y) >> 16);
            out++;
            count += 3;
            if (current == bez)
                break;
            out->x = (WORD)((current->anchor.x << 1) >> 16);
            out->y = (WORD)((current->anchor.y << 1) >> 16);
            out++;
            current--;
        }
        else
        {
            /* divide the section in two */
            mid.x = (current->in.x + current->out.x) >> 1;
            mid.y = (current->in.y + current->out.y) >> 1;
            (current + 2)->anchor.x = (current + 1)->anchor.x;
            (current + 2)->anchor.y = (current + 1)->anchor.y;
            (current + 1)->out.x = (current->out.x + (current + 1)->anchor.x) >> 1;
            (current + 1)->out.y = (current->out.y + (current + 1)->anchor.y) >> 1;
            (current + 1)->in.x = (mid.x + (current + 1)->out.x) >> 1;
            (current + 1)->in.y = (mid.y + (current + 1)->out.y) >> 1;
            current->in.x = (current->in.x + current->anchor.x) >> 1;
            current->in.y = (current->in.y + current->anchor.y) >> 1;
            current->out.x = (current->in.x + mid.x) >> 1;
            current->out.y = (current->in.y + mid.y) >> 1;
            (current + 1)->anchor.x = ((current + 1)->in.x + current->out.x) >> 1;
            (current + 1)->anchor.y = ((current + 1)->in.y + current->out.y) >> 1;

            current->level--;
            (current + 1)->level = current->level;
            (current + 1)->onscreen = current->onscreen;
            current++;
        }
    }

    return count;
}


/*
 * calc_bez - the points of the curves, lines and moves of PTSIN at a depth
 * scale, into bez_pts (with bez_break)
 *
 * 'close': each contour gets its first point again at its end (for filling).
 * Returns the number of points made, or -1 if they would be too many or if
 * there are neither curves nor moves ('*used' then tells which); '*moves'
 * is the number of move points.
 */
static WORD calc_bez(const Point *points, WORD count, WORD depth_scale, BOOL close,
                     const WORD *clip, WORD *moves, BOOL *used)
{
    WORD maxin, i, flags, out, first;
    LONG maxpnt = count;

    *moves = 0;
    *used = FALSE;

    /* the flags of the last point are not looked at; nor are those intin does not hold */
    maxin = count - 1;
    if (maxin > 2 * CONTRL[3])
        maxin = 2 * CONTRL[3];

    if (close)
        maxpnt++;
    for (i = 0; i < maxin; i++)
    {
        flags = bez_flags(i);
        if (flags & BEZ_MOVE)
        {
            (*moves)++;
            *used = TRUE;
        }
        if (flags & BEZ_START)
        {
            if (i >= maxin - 2)
                break;                  /* a curve needs its four points */
            *used = TRUE;
            maxpnt += (3L << (bez_depth(points + i, depth_scale) - 1)) - 3;
            i += 2;
        }
    }
    if (close)
        maxpnt += *moves;
    if (maxpnt > MAX_POINTS || !*used)
        return -1;

    memset(bez_break, 0, sizeof(bez_break));
    out = 0;
    first = 0;                          /* the first point of the contour, in PTSIN */
    for (i = 0; i < count; i++)
    {
        if (i < maxin)
        {
            flags = bez_flags(i);
            if (flags & BEZ_MOVE)
            {
                if (close)
                    bez_pts[out++] = points[first];
                first = i;
                SET_BREAK(out);
            }
            bez_pts[out++] = points[i];
            if (flags & BEZ_START)
            {
                if (i >= maxin - 2)
                    break;
                out += do_bez4(points + i, bez_depth(points + i, depth_scale), bez_pts + out, clip) - 1;
                i += 2;
            }
        }
        else
            bez_pts[out++] = points[i];
    }
    if (close)
        bez_pts[out++] = points[first];

    return out;
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
 * Make the points at the workstation's quality, or coarser where they would
 * be too many; -1 if they cannot be made (or there is nothing to make)
 */
static WORD make_points(Vwk *vwk, BOOL close, WORD *moves, BOOL *used)
{
    WORD clip[4], scale, n = -1;

    bez_clip(vwk, clip);
    for (scale = vwk->bez_qual; scale <= COARSEST_SCALE; scale++)
    {
        n = calc_bez((const Point *)PTSIN, CONTRL[1], scale, close, clip, moves, used);
        if (n >= 0 || !*used)
            break;
    }
    return n;
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
    WORD n, moves, i, start;
    BOOL used;

    n = make_points(vwk, FALSE, &moves, &used);
    if (n < 0)
    {
        /* nothing to make of it: a polyline, as fVDI; too many points: nothing */
        if (!used)
        {
            bez_polyline(vwk, (Point *)PTSIN, CONTRL[1]);
            moves = 0;
        }
        bez_answer((const Point *)PTSIN, CONTRL[1], moves);
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
    WORD n, moves, i, start;
    BOOL used;

    n = make_points(vwk, TRUE, &moves, &used);
    if (n < 0)
    {
        if (!used)
        {
            polygon(vwk, (Point *)PTSIN, CONTRL[1]);
            moves = 0;
        }
        bez_answer((const Point *)PTSIN, CONTRL[1], moves);
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
        vwk->bez_qual = 0;
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
