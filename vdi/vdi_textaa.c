/*
 * vdi_textaa.c - anti-aliased text: coverage blended into 16-bit pixels
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * Anti-aliased outline text (CONF_WITH_VDI_TEXT_AA)
 *
 * On a 16-bit screen, in the replace and transparent writing modes, the
 * outline font engine renders the glyphs with FreeType's smooth renderer,
 * 256 levels of coverage, and the text colour is blended into the pixels
 * by them: over what is there in transparent mode, over the text
 * background colour (pen 0, or vst_bg_color()) in replace mode, where the
 * one-bit text paints that colour too.  Everything else stays one-bit:
 * the xor and reverse transparent modes, where a blend means nothing, and
 * screens of 8 planes or fewer, which have no colours between.
 *
 * The effects are the engine's in both kinds: bold and outlined glyphs are
 * made from the outline, italic and vst_skew() slant it, the underline is
 * a band of full coverage.  Light text, which the one-bit text draws in
 * alternate pixels, has half its coverage: grey, as smooth as the rest.
 * Outlined text keeps the engine's thin outline, half a pixel wide, so it
 * is lighter than the one-bit text, which draws such lines a pixel wide.
 * Rotated text is turned, as the one-bit text is.  Sizes, advances and
 * extents do not change: both kinds come from the same hinted glyphs.
 * Where glyphs overlap, their coverages add up as independent ones do.
 *
 * The blend is linear in the pixel values, without gamma, in 32 steps (the
 * pixels have 5 bits a component): coverage c is step (c + 4) / 8, and a
 * component goes from d to d + (f - d) * step / 32, rounded.  So no
 * coverage leaves a pixel alone, and full coverage gives the text colour.
 *
 * The engine keeps the coverage images of the glyphs as it keeps the
 * one-bit ones, within 256 KB (COVER_BUDGET in vdi_ft2.c).  A string is
 * rendered into a buffer of a byte a pixel, eight times the one-bit one;
 * when that cannot be had, the string is drawn one-bit.
 *
 * The switch: neither NVDI nor fVDI has a call or mode bit for it (fVDI
 * reads "antialias" in FVDI.SYS when it starts), so it stays internal.
 * vdi_text_mono, FALSE at boot, makes all outline text one-bit when set;
 * a host may set it from its own configuration.
 *
 * This file touches nothing but its arguments, and every number in it is
 * a WORD, a LONG or their unsigned kinds: it is checked on the host.
 */

#include "portab.h"
#include "vdi_textaa.h"

#define RED(p)      ((UWORD)((p) >> 11))
#define GREEN(p)    ((UWORD)(((p) >> 6) & 0x1f))
#define BLUE(p)     ((UWORD)((p) & 0x1f))

/*
 * one component: d + (f - d) * level / 32, rounded, without a negative
 * number: (f + 32 - d) * level is (f - d) * level + 32 * level
 */
static UWORD mix5(UWORD d, UWORD f, UWORD level)
{
    return (UWORD)(d + ((UWORD)((f + VDI_AA_LEVELS - d) * level + VDI_AA_LEVELS / 2) >> 5) - level);
}


UWORD vdi_aa_mix16(UWORD d, UWORD f, UWORD level)
{
    if (level == 0)
        return d;
    if (level >= VDI_AA_LEVELS)
        return f;

    return (UWORD)((mix5(RED(d), RED(f), level) << 11)
                 | (mix5(GREEN(d), GREEN(f), level) << 6)
                 | mix5(BLUE(d), BLUE(f), level));
}


/* the blends of transparent mode, from the colour last found under the text */
typedef struct {
    UWORD ramp[VDI_AA_LEVELS];  /* [n]: step n from under to the text colour */
    ULONG known;                /* bit n: ramp[n] is made */
    UWORD under;
    UWORD fg;
} Trans;

static void trans_pixel(Trans *t, UWORD *d, UWORD level)
{
    if (level == 0)
        return;
    if (level >= VDI_AA_LEVELS)
    {
        *d = t->fg;
        return;
    }
    if (*d != t->under)
    {
        t->under = *d;
        t->known = 0;
    }
    if (!(t->known & (1UL << level)))
    {
        t->ramp[level] = vdi_aa_mix16(t->under, t->fg, level);
        t->known |= 1UL << level;
    }
    *d = t->ramp[level];
}


/* four coverages under 4, step 0 each, in a long */
#define BLANK4(q)   (((q) & 0xfcfcfcfcUL) == 0)

/*
 * Most of a string's box is blank: where the coverage is aligned, it is
 * looked at a long, four pixels, at a time, and four blank pixels are
 * left (or painted the background colour) at once.  In transparent mode
 * each step is mixed once from the colour last found under the text: text
 * on a plain background mixes each step once.
 */
void vdi_aa_blend16(UWORD *dst, LONG dst_bytes, const UBYTE *cov, LONG cov_bytes,
                    LONG width, LONG height, UWORD fg, UWORD bg, WORD mode)
{
    UWORD ramp[VDI_AA_LEVELS + 1];
    Trans t;
    LONG x, y;

    if (mode == VDI_AA_REPLACE)
    {
        UWORD level;

        for (level = 0; level <= VDI_AA_LEVELS; level++)
            ramp[level] = vdi_aa_mix16(bg, fg, level);
    }
    t.known = 0;
    t.under = bg;
    t.fg = fg;

    for (y = 0; y < height; y++)
    {
        UWORD *d = dst;
        const UBYTE *c = cov;

        x = width;
        if (mode == VDI_AA_REPLACE)
        {
            for ( ; x > 0 && ((ULONG)c & 3); x--)
                *d++ = ramp[VDI_AA_LEVEL(*c++)];
            for ( ; x >= 4; x -= 4, c += 4, d += 4)
            {
                if (BLANK4(*(const ULONG_ALIAS *)c))
                {
                    d[0] = d[1] = d[2] = d[3] = bg;
                    continue;
                }
                d[0] = ramp[VDI_AA_LEVEL(c[0])];
                d[1] = ramp[VDI_AA_LEVEL(c[1])];
                d[2] = ramp[VDI_AA_LEVEL(c[2])];
                d[3] = ramp[VDI_AA_LEVEL(c[3])];
            }
            for ( ; x > 0; x--)
                *d++ = ramp[VDI_AA_LEVEL(*c++)];
        }
        else
        {
            for ( ; x > 0 && ((ULONG)c & 3); x--)
                trans_pixel(&t, d++, VDI_AA_LEVEL(*c++));
            for ( ; x >= 4; x -= 4, c += 4, d += 4)
            {
                if (BLANK4(*(const ULONG_ALIAS *)c))
                    continue;
                trans_pixel(&t, d, VDI_AA_LEVEL(c[0]));
                trans_pixel(&t, d + 1, VDI_AA_LEVEL(c[1]));
                trans_pixel(&t, d + 2, VDI_AA_LEVEL(c[2]));
                trans_pixel(&t, d + 3, VDI_AA_LEVEL(c[3]));
            }
            for ( ; x > 0; x--)
                trans_pixel(&t, d++, VDI_AA_LEVEL(*c++));
        }

        dst = (UWORD *)((UBYTE *)dst + dst_bytes);
        cov += cov_bytes;
    }
}


/* x,y goes to y,w-1-x (900), w-1-x,h-1-y (1800) or h-1-y,x (2700) */
void vdi_aa_turn(const UBYTE *src, LONG src_bytes, LONG w, LONG h, WORD angle,
                 UBYTE *dst, LONG dst_bytes)
{
    LONG x, y;

    for (y = 0; y < h; y++, src += src_bytes)
    {
        UBYTE *p;

        switch (angle)
        {
        case 900:
            p = dst + (w - 1) * dst_bytes + y;
            for (x = 0; x < w; x++, p -= dst_bytes)
                *p = src[x];
            break;
        case 1800:
            p = dst + (h - 1 - y) * dst_bytes + w - 1;
            for (x = 0; x < w; x++)
                *p-- = src[x];
            break;
        default:        /* 2700 */
            p = dst + (h - 1 - y);
            for (x = 0; x < w; x++, p += dst_bytes)
                *p = src[x];
            break;
        }
    }
}
