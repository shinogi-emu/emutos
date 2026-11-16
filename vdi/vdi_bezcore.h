/*
 * vdi_bezcore.h - the points of Bezier curves, as fVDI makes them
 *
 * Pure functions: no hardware, no VDI globals.  vdi_bezier.c copies the
 * flags out of intin, calls them and draws the points; the host tests build
 * vdi_bezcore.c on its own.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_BEZCORE_H
#define VDI_BEZCORE_H

#include "portab.h"
#include "config.h"

/* a point, as the VDI's Point */
typedef struct
{
    WORD x;
    WORD y;
} BezPoint;

#define BEZ_START       0x01    /* the point and the next three are a curve */
#define BEZ_MOVE        0x02    /* the point starts a new polyline (or contour) */

#define BEZ_FINEST      0       /* the depth scale of v_bez_qual() */
#define BEZ_COARSEST    9
#define BEZ_MAX_DEPTH   7       /* a section is halved at most this deep */

/*
 * The input: 'count' points, and the flags of the first 'nflags' of them
 * (at most count - 1; the others have none), a byte each.  A curve takes the
 * point with BEZ_START and the next three; one that does not have its four
 * points is not a curve (that point and those after it are dropped from the
 * curves and moves, as fVDI drops them; if nothing else was a curve or a
 * move, the points are a plain polyline).  Neither function reads beyond
 * pts[count - 1] or flags[nflags - 1].
 *
 * bez_count(): the points the curves would need at most at a depth scale
 * (with 'close': each contour gets its first point again at its end), and
 * the number of move points; *used is FALSE if there is neither a curve nor
 * a move (the points are then a polyline as they are).
 */
LONG bez_count(const BezPoint *pts, WORD count, const UBYTE *flags, WORD nflags,
               WORD scale, BOOL close, WORD *moves, BOOL *used);

/*
 * bez_generate(): make them into out[], at most 'room' points, and set bit i
 * of brk (byte i / 8, the top bit first; (room + 8) / 8 bytes, cleared here)
 * for each point that starts a new polyline.  'clip' (x1, y1, x2, y2): the
 * parts of a curve outside it are divided less finely.  Returns the number
 * of points, or -1 if they would be more than 'room' (nothing is written past
 * out[room - 1] in any case).
 */
WORD bez_generate(const BezPoint *pts, WORD count, const UBYTE *flags, WORD nflags,
                  WORD scale, BOOL close, const WORD *clip,
                  BezPoint *out, UBYTE *brk, WORD room);

#endif /* VDI_BEZCORE_H */
