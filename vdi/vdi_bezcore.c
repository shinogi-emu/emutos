/*
 * vdi_bezcore.c - the points of Bezier curves, as fVDI makes them
 *
 * fVDI's subdivision (engine/bezier.c): bez_depth(), do_bez4() and the
 * counting and making of the points of calc_bez(), split in two (counting,
 * making) so that the VDI can try coarser scales, and with every write of
 * the making bounded by the room given (the count is an estimate made from
 * the same input; the bound is what the memory relies on).  The flags are
 * the caller's copy.  The file has no VDI globals and no plain ints
 * (EmuTOS has 16-bit ones).
 *
 * Copyright 1999-2001/2003, Johan Klockars (fVDI)
 * Copyright 1999, Caldera Thin Clients, Inc.
 * Copyright (C) 2026 The EmuTOS development team
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

#include "vdi_bezcore.h"

#if CONF_WITH_VDI_BEZIER

#define BEZ_MIN_DEPTH   2       /* a curve is never made of fewer than 3 << 1 lines */


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
static WORD bez_depth(const BezPoint *p, WORD depth_scale)
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
#define FIXED(c)    ((LONG)(c) * 32768L + 0x4000)   /* fVDI: (c << 16) >> 1, without a shift of a negative */

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
static WORD do_bez4(const BezPoint *in, WORD depth, BezPoint *out, const WORD *clip)
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
            out->x = (WORD)((current->anchor.x * 2) >> 16);
            out->y = (WORD)((current->anchor.y * 2) >> 16);
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


LONG bez_count(const BezPoint *pts, WORD count, const UBYTE *flags, WORD nflags,
               WORD scale, BOOL close, WORD *moves, BOOL *used)
{
    LONG maxpnt = count;
    WORD i, f;

    *moves = 0;
    *used = FALSE;
    if (nflags > count - 1)
        nflags = count - 1;
    if (close)
        maxpnt++;
    for (i = 0; i < nflags; i++)
    {
        f = flags[i];
        if (f & BEZ_MOVE)
        {
            (*moves)++;
            *used = TRUE;
        }
        if (f & BEZ_START)
        {
            if (i >= nflags - 2)
                break;                  /* a curve needs its four points */
            *used = TRUE;
            maxpnt += (3L << (bez_depth(pts + i, scale) - 1)) - 3;
            i += 2;
        }
    }
    if (close)
        maxpnt += *moves;
    return maxpnt;
}


#define SET_BREAK(brk, i)   ((brk)[(i) >> 3] |= (UBYTE)(0x80 >> ((i) & 7)))

WORD bez_generate(const BezPoint *pts, WORD count, const UBYTE *flags, WORD nflags,
                  WORD scale, BOOL close, const WORD *clip,
                  BezPoint *out, UBYTE *brk, WORD room)
{
    WORD i, f, n = 0, first = 0, depth;

    for (i = 0; i < (room + 8) / 8; i++)
        brk[i] = 0;
    if (count <= 0)
        return 0;
    if (nflags > count - 1)
        nflags = count - 1;
    for (i = 0; i < count; i++)
    {
        f = (i < nflags) ? flags[i] : 0;
        if (f & BEZ_MOVE)
        {
            if (close)
            {
                if (n >= room)
                    return -1;
                out[n++] = pts[first];  /* the contour is closed */
            }
            first = i;
            SET_BREAK(brk, n);
        }
        if (n >= room)
            return -1;
        out[n++] = pts[i];
        if (f & BEZ_START)
        {
            if (i >= nflags - 2)
                break;                  /* a curve needs its four points */
            depth = bez_depth(pts + i, scale);
            /* do_bez4() makes at most 3 << (depth - 1) points less one */
            if ((LONG)n + (3L << (depth - 1)) - 1 > room)
                return -1;
            n += do_bez4(pts + i, depth, out + n, clip) - 1;
            i += 2;
        }
    }
    if (close)
    {
        if (n >= room)
            return -1;
        out[n++] = pts[first];
    }
    return n;
}

#endif /* CONF_WITH_VDI_BEZIER */
