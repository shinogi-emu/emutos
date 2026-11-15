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
