/*
 * vdi_nvdimath.c - the arithmetic of the NVDI colour calls
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "vdi_nvdimath.h"

UWORD vdi_to_rgb16(WORD value)
{
    if (value <= 0)
        return 0;
    if (value >= 1000)
        return 0xffff;
    return (UWORD)(((ULONG)value * 65535UL + 500UL) / 1000UL);
}


WORD rgb16_to_vdi(UWORD value)
{
    return (WORD)(((ULONG)value * 1000UL + 32767UL) / 65535UL);
}


UWORD vdi_pack16(UWORD red, UWORD green, UWORD blue)
{
    return (UWORD)(((red >> 11) << 11) | ((green >> 11) << 6) | (blue >> 11));
}


/* five bits of 'v' as 16, the top bits repeated: v v v v, less the lowest four bits */
static UWORD widen5(UWORD v)
{
    v &= 0x1f;
    return (UWORD)((v << 11) | (v << 6) | (v << 1) | (v >> 4));
}


void vdi_unpack16(UWORD pixel, UWORD *red, UWORD *green, UWORD *blue)
{
    *red = widen5(pixel >> 11);
    *green = widen5(pixel >> 6);
    *blue = widen5(pixel);
}


static WORD clamp1000(WORD v)
{
    if (v < 0)
        return 0;
    if (v > 1000)
        return 1000;
    return v;
}


LONG vdi_color_dist(const WORD *a, const WORD *b)
{
    LONG dr = (LONG)clamp1000(a[0]) - clamp1000(b[0]);
    LONG dg = (LONG)clamp1000(a[1]) - clamp1000(b[1]);
    LONG db = (LONG)clamp1000(a[2]) - clamp1000(b[2]);

    return dr * dr + dg * dg + db * db;
}


WORD vdi_nearest_index(const WORD (*table)[3], WORD count, const WORD *rgb, LONG *best)
{
    WORD i, found = -1;
    LONG d;

    for (i = 0; i < count; i++)
    {
        d = vdi_color_dist(table[i], rgb);
        if (d < *best)
        {
            *best = d;
            found = i;
        }
    }
    return found;
}


/* a * b as hi:lo, the 32x32 product from 16-bit pieces */
static void mul32(ULONG a, ULONG b, ULONG *hi, ULONG *lo)
{
    ULONG al = a & 0xffffUL, ah = a >> 16;
    ULONG bl = b & 0xffffUL, bh = b >> 16;
    ULONG ll = al * bl;
    ULONG lh = al * bh;
    ULONG hl = ah * bl;
    ULONG hh = ah * bh;
    ULONG mid = (ll >> 16) + (lh & 0xffffUL) + (hl & 0xffffUL);

    *lo = (ll & 0xffffUL) | (mid << 16);
    *hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}


/* (a * b - less) / c, for c > 0 and a * b >= less; the quotient capped at 0x7fffffff */
static LONG muldiv_less(ULONG a, ULONG b, ULONG c, ULONG less)
{
    ULONG hi, lo, rem, quo;
    WORD i;

    mul32(a, b, &hi, &lo);
    if (lo < less)
        hi--;
    lo -= less;
    if (hi == 0)
        quo = lo / c;
    else if (hi >= c)
        return 0x7fffffffL;     /* too big for a LONG */
    else
    {
        /* restoring division of hi:lo, hi < c <= 0x7fffffff */
        rem = hi;
        quo = 0;
        for (i = 0; i < 32; i++)
        {
            rem = (rem << 1) | (lo >> 31);
            lo <<= 1;
            quo <<= 1;
            if (rem >= c)
            {
                rem -= c;
                quo |= 1;
            }
        }
    }
    return quo > 0x7fffffffUL ? 0x7fffffffL : (LONG)quo;
}


LONG muldiv32(LONG a, LONG b, LONG c)
{
    if (a < 0 || b <= 0 || c <= 0)
        return 0;
    return muldiv_less((ULONG)a, (ULONG)b, (ULONG)c, 0UL);
}


/* the span a1..a2 as a count, or 0 if it is upside down or over 0x7fffffff */
static ULONG span(LONG a1, LONG a2)
{
    ULONG n;

    if (a1 > a2)
        return 0;
    n = (ULONG)a2 - (ULONG)a1 + 1UL;   /* 0 for the full 32 bits: refused too */
    return n > 0x7fffffffUL ? 0 : n;
}


/* [n1..n2] of a1..a2 onto b1..b2 (sa and sb their counts, both within a LONG) */
static void bring_along(LONG a1, ULONG sa, LONG n1, LONG n2, LONG b1, ULONG sb,
                        LONG *m1, LONG *m2)
{
    ULONG d1 = (ULONG)n1 - (ULONG)a1;           /* 0 to sa - 1 */
    ULONG d2 = (ULONG)n2 - (ULONG)a1 + 1UL;     /* 1 to sa */
    ULONG r1, r2;

    if (sa == sb)
    {
        r1 = d1;
        r2 = d2 - 1UL;
    }
    else
    {
        r1 = (ULONG)muldiv_less(d1, sb, sa, 0UL);
        r2 = (ULONG)muldiv_less(d2, sb, sa, 1UL);
    }
    *m1 = (LONG)((ULONG)b1 + r1);
    *m2 = (LONG)((ULONG)b1 + r2);
}


WORD clip_pair(const VDI_RECT32 *clip, const VDI_RECT32 *clipped_side,
               const VDI_RECT32 *other, VDI_RECT32 *out_clipped,
               VDI_RECT32 *out_other)
{
    VDI_RECT32 n;
    ULONG sax, say, sbx, sby;

    n.x1 = clipped_side->x1 > clip->x1 ? clipped_side->x1 : clip->x1;
    n.y1 = clipped_side->y1 > clip->y1 ? clipped_side->y1 : clip->y1;
    n.x2 = clipped_side->x2 < clip->x2 ? clipped_side->x2 : clip->x2;
    n.y2 = clipped_side->y2 < clip->y2 ? clipped_side->y2 : clip->y2;
    if (n.x1 > n.x2 || n.y1 > n.y2)
        return 0;
    sax = span(clipped_side->x1, clipped_side->x2);
    say = span(clipped_side->y1, clipped_side->y2);
    sbx = span(other->x1, other->x2);
    sby = span(other->y1, other->y2);
    if (!sax || !say || !sbx || !sby)
        return 0;

    *out_clipped = n;
    bring_along(clipped_side->x1, sax, n.x1, n.x2, other->x1, sbx, &out_other->x1, &out_other->x2);
    bring_along(clipped_side->y1, say, n.y1, n.y2, other->y1, sby, &out_other->y1, &out_other->y2);
    return 1;
}
