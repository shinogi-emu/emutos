/*
 * vdi_alpha.c - images with alpha: blending into 16-bit pixels
 *
 * A source pixel of 8-bit components with an alpha A (straight alpha: the
 * colour is not multiplied by it) is blended into a 16-bit pixel by the
 * rule below, which gives the same pixels everywhere it is implemented.
 * It has no division; the mix has one 16-bit product per component and
 * nothing over 16256, and kc (a product reaching 2^24) is one 16 x 16 to 32
 * bit multiply, mulu.w, with a weight below full (at full weight, none).
 * So it suits the 68000.  The file has no VDI globals and no
 * plain ints (EmuTOS has 16-bit ones), and is checked on the host against
 * shared vectors and an independent reference (tests/vdi-nvdi).
 *
 * For each component c of red, green and blue, with the weight Wc of
 * vs_weight_color() (0-65535), the source alpha A (0-255), the destination
 * field of n bits (5 or 6) holding d and the 8-bit source component C:
 *
 *     Wc' = Wc + (Wc >> 15)                0-65536, once per call
 *     wa  = A + (A >> 7)                   0-256
 *     kc  = (wa * Wc' + 32768) >> 16       0-256
 *     s   = C >> (8 - n)                   the cut modes 0 and 32 make
 *     out = (d * 256 + (s - d) * kc + 128) >> 8
 *
 * A source pixel with A = 0 is not touched.  Any other is written, with
 * kc = 256 (A = 255 at full weight) giving the pixel mode 32 writes.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "vdi_alpha.h"


ULONG vdi_alpha_weight(UWORD w)
{
    return (ULONG)w + (w >> 15);
}


UWORD vdi_alpha_k(UWORD alpha, ULONG wprime)
{
    UWORD wa = alpha + (alpha >> 7);
    UWORD w = (UWORD)wprime;            /* 0 for 65536 */
    ULONG p = (ULONG)wa * w;            /* two UWORDs: one mulu.w, whatever follows */

    /* 65536 is the one weight that is not a UWORD, and keeps wa whole */
    if (wprime & VDI_ALPHA_FULL)
        return wa;
    return (UWORD)((p + 32768UL) >> 16);
}


/* one field of n bits: d towards the component's top bits s by k/256, rounded */
#define MIX(d, s, k)    ((UWORD)(((WORD)((d) << 8) + (WORD)(((WORD)(s) - (WORD)(d)) * (WORD)(k)) + 128) >> 8))

UWORD vdi_alpha_mix565(UWORD d, ULONG rgb, UWORD kr, UWORD kg, UWORD kb)
{
    UWORD r, g, b;

    r = MIX((d >> 11) & 31, (UWORD)(rgb >> 19) & 31, kr);
    g = MIX((d >> 5) & 63, (UWORD)(rgb >> 10) & 63, kg);
    b = MIX(d & 31, (UWORD)(rgb >> 3) & 31, kb);
    return (UWORD)((r << 11) | (g << 5) | b);
}


#if defined(__m68k__)
#define LOAD16(p)           (*(const UWORD_ALIAS *)(p))
#define STORE16(out, v)     (*(UWORD_ALIAS *)(out) = (UWORD)(v))
#define LOAD32(p)           (*(const ULONG_ALIAS *)(p))
#else
#define LOAD16(p)           ((UWORD)(((UWORD)(p)[0] << 8) | (p)[1]))
#define STORE16(out, v)     do { (out)[0] = (UBYTE)((v) >> 8); (out)[1] = (UBYTE)(v); } while (0)
#define LOAD32(p)           (((ULONG)(p)[0] << 24) | ((ULONG)(p)[1] << 16) | ((ULONG)(p)[2] << 8) | (p)[3])
#endif


/* a pixel of mode 32: the colour cut to 5-6-5 */
#define PACK565(rgb)        ((UWORD)((((rgb) >> 8) & 0xf800UL) | (((rgb) >> 5) & 0x07e0UL) | (((rgb) >> 3) & 31UL)))


void vdi_alpha_row565(UBYTE *dst, const UBYTE *src, LONG count, BOOL alpha_first, const ULONG wp[3])
{
    BOOL full = wp[0] >= VDI_ALPHA_FULL && wp[1] >= VDI_ALPHA_FULL && wp[2] >= VDI_ALPHA_FULL;
    ULONG px, rgb;
    UWORD a, v, kr, kg, kb;

    for (; count > 0; count--, src += 4, dst += 2)
    {
        px = LOAD32(src);
        if (alpha_first)
        {
            a = (UWORD)(px >> 24);
            rgb = px & 0x00ffffffUL;
        }
        else
        {
            a = (UWORD)(px & 255);
            rgb = px >> 8;
        }
        if (a == 0)
            continue;                   /* not read, not written */
        if (full)
        {
            if (a == 255)
            {
                v = PACK565(rgb);
                STORE16(dst, v);
                continue;
            }
            kr = kg = kb = a + (a >> 7);
        }
        else
        {
            kr = vdi_alpha_k(a, wp[0]);
            kg = vdi_alpha_k(a, wp[1]);
            kb = vdi_alpha_k(a, wp[2]);
        }
        v = vdi_alpha_mix565(LOAD16(dst), rgb, kr, kg, kb);
        STORE16(dst, v);
    }
}
