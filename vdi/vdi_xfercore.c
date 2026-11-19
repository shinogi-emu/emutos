/*
 * vdi_xfercore.c - the pixel conversion of vr_transfer_bits
 *
 * Ported from fVDI's transfer.c (rational stepping that does not restart
 * its phase when the destination is clipped, a four-sample filter for
 * reductions onto 16 bits), reduced to the slice EmuTOS serves: sources of
 * 1, 8, 15, 16, 24 and 32 bits, destinations of 16-bit words and of 1, 2,
 * 4 and 8 interleaved planes.  With CONF_WITH_VDI_ALPHA, also 32-bit
 * sources with alpha (ARGB, RGBA), and the blend of direct sources onto
 * 16-bit words (mode T_BLEND, the arithmetic in vdi_alpha.c).  The file has
 * no VDI globals and no plain ints (EmuTOS has 16-bit ones).
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "vdi_xfercore.h"
#if CONF_WITH_VDI_ALPHA
#include "vdi_alpha.h"
#endif

/* a 16-bit destination exists only in a build with 16-bit graphics */
#if CONF_WITH_VDI_16BIT
#define IS_D16(planes)  ((planes) == XD_16)
#else
#define IS_D16(planes)  0
#endif

WORD xfer_kind(ULONG px_format)
{
    ULONG base = px_format & ~XF_PX_REVERSED;
    WORD rev = (px_format & XF_PX_REVERSED) ? XK_REV : 0;

    if (px_format == XF_PX_1)
        return XK_1;
    if (px_format == XF_PX_8)
        return XK_8;
    if (px_format == XF_PX_32)
        return XK_32;
#if CONF_WITH_VDI_ALPHA
    if (px_format == XF_PX_ARGB32)
        return XK_ARGB32;
    if (px_format == XF_PX_RGBA32)
        return XK_RGBA32;
#endif
    if (base == XF_PX_15)
        return XK_15 | rev;
    if (base == XF_PX_16)
        return XK_16 | rev;
    if (base == XF_PX_24)
        return XK_24 | rev;
    return XK_NONE;
}


WORD xfer_kind_bits(WORD kind)
{
    switch (kind & XK_MASK)
    {
    case XK_1:
        return 1;
    case XK_8:
        return 8;
    case XK_15:
    case XK_16:
        return 16;
    case XK_24:
        return 24;
    case XK_32:
#if CONF_WITH_VDI_ALPHA
    case XK_ARGB32:
    case XK_RGBA32:
#endif
        return 32;
    default:
        return 0;
    }
}


ULONG xfer_unpack(const UBYTE *p, WORD kind)
{
    ULONG v, r, g, b;

    switch (kind & XK_MASK)
    {
    case XK_32:
#if CONF_WITH_VDI_ALPHA
    case XK_ARGB32:
#endif
        return ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
#if CONF_WITH_VDI_ALPHA
    case XK_RGBA32:
        return ((ULONG)p[0] << 16) | ((ULONG)p[1] << 8) | p[2];
#endif
    case XK_24:
        if (kind & XK_REV)
            return ((ULONG)p[2] << 16) | ((ULONG)p[1] << 8) | p[0];
        return ((ULONG)p[0] << 16) | ((ULONG)p[1] << 8) | p[2];
    case XK_16:
    case XK_15:
        v = (kind & XK_REV) ? (((ULONG)p[1] << 8) | p[0]) : (((ULONG)p[0] << 8) | p[1]);
        b = v & 31;
        if ((kind & XK_MASK) == XK_16)
        {
            r = v >> 11;
            g = (v >> 5) & 63;
            g = (g << 2) | (g >> 4);
        }
        else
        {
            r = (v >> 10) & 31;
            g = (v >> 5) & 31;
            g = (g << 3) | (g >> 2);
        }
        return (((r << 3) | (r >> 2)) << 16) | (g << 8) | (b << 3) | (b >> 2);
    default:
        return 0;
    }
}


UWORD xfer_pack565(ULONG rgb)
{
    return (UWORD)(((rgb >> 8) & 0xf800UL) | ((rgb >> 5) & 0x07e0UL) | ((rgb >> 3) & 31UL));
}


#if CONF_WITH_VDI_ALPHA
UWORD xfer_alpha(const UBYTE *p, WORD kind)
{
    switch (kind & XK_MASK)
    {
    case XK_ARGB32:
        return p[0];
    case XK_RGBA32:
        return p[3];
    default:
        return 255;
    }
}


BOOL xfer_blend_ok(WORD kind, WORD planes)
{
    WORD k = kind & XK_MASK;

    return k != XK_NONE && k != XK_1 && k != XK_8 && IS_D16(planes);
}
#endif


/* the bytes of a line of 'width' pixels of a source kind, at least; 0 for a bad kind */
static LONG src_line_min(WORD kind, LONG width)
{
    switch (kind & XK_MASK)
    {
    case XK_1:
        return (width + 7) / 8;
    case XK_8:
        return width;
    case XK_15:
    case XK_16:
        return width * 2;
    case XK_24:
        return width * 3;
    case XK_32:
#if CONF_WITH_VDI_ALPHA
    case XK_ARGB32:
    case XK_RGBA32:
#endif
        return width * 4;
    default:
        return 0;
    }
}


/*
 * the size of the memory of a bitmap whose lines have at least 'line_min'
 * bytes; 'even': the address and the line width must be even (words are
 * read and written there); 0 if it is not usable
 */
static ULONG bitmap_size(const GCBITMAP *bm, LONG (*line_min)(WORD, LONG), WORD arg, BOOL even)
{
    LONG w, h;

    if (!bm->addr || bm->xmin < -32768L || bm->ymin < -32768L ||
        bm->xmax > 32768L || bm->ymax > 32768L ||
        bm->xmax <= bm->xmin || bm->ymax <= bm->ymin)
        return 0;
    if (even && ((((ULONG)bm->addr) | (ULONG)bm->width) & 1UL))
        return 0;
    w = bm->xmax - bm->xmin;
    h = bm->ymax - bm->ymin;
    if (bm->width < line_min(arg, w) || bm->width > 0x7fffffffL / h)
        return 0;
    return (ULONG)bm->width * (ULONG)h;
}


ULONG xfer_src_size(const GCBITMAP *bm, WORD kind)
{
    WORD bits = xfer_kind_bits(kind);
    WORD k = kind & XK_MASK;

    if (bits == 0 || bm->bits != bits)
        return 0;
#if CONF_WITH_VDI_ALPHA
    if (k == XK_ARGB32 || k == XK_RGBA32)
        k = XK_32;                      /* words and longs are read there too */
#endif
    return bitmap_size(bm, src_line_min, kind, k == XK_15 || k == XK_16 || k == XK_32);
}


/* the bytes of a line of 'width' pixels of a destination of 'planes' planes */
static LONG dst_line_min(WORD planes, LONG width)
{
    if (IS_D16(planes))
        return width * 2;
    if (planes == XD_INDEX8)
        return width;
    return ((width + 15) / 16) * 2 * planes;
}


ULONG xfer_dst_size(const GCBITMAP *bm, WORD planes)
{
    if (planes == XD_INDEX8)
    {
        if (bm->bits != 8 || bm->px_format != XF_PX_8)
            return 0;
        return bitmap_size(bm, dst_line_min, planes, FALSE);
    }
    if (!IS_D16(planes) && planes != 1 && planes != 2 && planes != 4 && planes != 8)
        return 0;
    if (bm->bits != planes)
        return 0;
    return bitmap_size(bm, dst_line_min, planes, IS_D16(planes));
}


BOOL xfer_rect_in(const GCBITMAP *bm, const RECT16 *r)
{
    return r->x1 <= r->x2 && r->y1 <= r->y2 &&
           r->x1 >= bm->xmin && r->y1 >= bm->ymin &&
           (LONG)r->x2 < bm->xmax && (LONG)r->y2 < bm->ymax;
}


BOOL xfer_overlap(const void *a, ULONG alen, const void *b, ULONG blen)
{
    ULONG aa = (ULONG)a, bb = (ULONG)b;

    return aa <= bb ? bb - aa < alen : aa - bb < blen;
}


/*
 * Exact rational stepping: clipping does not restart the sampling phase.
 * With 'filtered' the position is in 1/256 pixels, centred on the pixel.
 * Products stay below 2^32 for sizes up to 65536 and skips below the size
 * of the destination; there is no division per pixel.
 */
void xfer_axis_start(XferAxis *a, ULONG source, ULONG dest, ULONG skip, BOOL filtered)
{
    ULONG n = skip * source;

    if (filtered)
    {
        n += source / 2;
        a->pos = (LONG)(n / dest) * 256 - 128;
        n = (n % dest) * 256 + (source & 1) * 128;
        a->pos += (LONG)(n / dest);
        source *= 256;
    }
    else
        a->pos = (LONG)(n / dest);
    a->error = n % dest;
    a->step = source / dest;
    a->remainder = source % dest;
    a->denominator = dest;
}


#define AXIS_NEXT(a)                    \
    do {                                \
        (a).pos += (LONG)(a).step;      \
        (a).error += (a).remainder;     \
        if ((a).error >= (a).denominator) \
        {                               \
            (a).error -= (a).denominator; \
            (a).pos++;                  \
        }                               \
    } while (0)


void xfer_axis_next(XferAxis *a)
{
    AXIS_NEXT(*a);
}


void xfer_begin(XferState *s, const XferJob *job)
{
    WORD i, k = job->kind & XK_MASK;
    BOOL reduced = job->sw > job->dw || job->sh > job->dh;
    ULONG skipx = job->skipx, skipy = job->skipy;

    s->job = job;
    s->filtered = FALSE;
    s->dskip = 0;
    if (job->index)
    {
        /* as fVDI: indexed sources are filtered only when dithered */
        if (reduced && (k != XK_8 || job->dither))
            s->filtered = TRUE;
        if (job->dither)
        {
            /* the error runs through every pixel of the rectangle from its top left */
            s->dskip = skipy;
            skipx = skipy = 0;
        }
    }
    else if (k != XK_1 && k != XK_8 && IS_D16(job->planes) && reduced)
        s->filtered = TRUE;
#if CONF_WITH_VDI_ALPHA
    if (job->blend)
        s->filtered = FALSE;            /* the nearest pixel: straight alpha is not filtered */
#endif
    xfer_axis_start(&s->x0, job->sw, job->dw, skipx, s->filtered);
    xfer_axis_start(&s->y, job->sh, job->dh, skipy, s->filtered);
    s->prev_y = -1;
    for (i = 0; i < 8; i++)
    {
        s->ckey[i] = 0xffffffffUL;
        s->cval[i] = 0;
    }
}


/*
 * Bilinear interpolation at the centres of the pixels, for reductions.
 * Eight fractional bits keep the arithmetic within 32 bits; it is a
 * four-sample filter, not an area filter.
 */
static ULONG blend_rgb(ULONG a, ULONG b, ULONG c, ULONG d, LONG fx, LONG fy)
{
    ULONG rgb = 0;
    LONG top, bottom, channel;
    WORD shift;

    for (shift = 16; shift >= 0; shift -= 8)
    {
        channel = (LONG)((a >> shift) & 255);
        top = channel * 256 + ((LONG)((b >> shift) & 255) - channel) * fx;
        channel = (LONG)((c >> shift) & 255);
        bottom = channel * 256 + ((LONG)((d >> shift) & 255) - channel) * fx;
        channel = (top * 256 + (bottom - top) * fy + 32768L) >> 16;
        rgb |= (ULONG)channel << shift;
    }
    return rgb;
}


static ULONG interpolate(const UBYTE *row0, const UBYTE *row1, LONG x, LONG width,
                         LONG fy, WORD kind, LONG pixel_bytes)
{
    ULONG a, b, c, d;
    LONG fx;

    if (x < 0)
        x = 0;
    if (x > (width - 1) * 256)
        x = (width - 1) * 256;
    fx = x & 255;
    x = (x >> 8) * pixel_bytes;
    a = xfer_unpack(row0 + x, kind);
    c = xfer_unpack(row1 + x, kind);
    if (fx)
    {
        b = xfer_unpack(row0 + x + pixel_bytes, kind);
        d = xfer_unpack(row1 + x + pixel_bytes, kind);
    }
    else
    {
        b = a;
        d = c;
    }
    return blend_rgb(a, b, c, d, fx, fy);
}


/* the pixel value for a colour: the last answers are kept */
static ULONG pen_for(XferState *s, ULONG rgb)
{
    WORD slot = (WORD)((rgb ^ (rgb >> 8) ^ (rgb >> 16)) & 7);

    if (s->ckey[slot] != rgb)
    {
        s->cval[slot] = (UWORD)s->job->pen_rgb(s->job->ctx, rgb);
        s->ckey[slot] = rgb;
    }
    return s->cval[slot];
}


/* the pixel value of an index of an 8-bit source onto planes */
static ULONG pen_for_index(const XferJob *j, UBYTE index)
{
    UWORD v = j->map[index];

    if (v == 0xffff)
    {
        v = (UWORD)j->pen_index(j->ctx, index);
        j->map[index] = v;
    }
    return v;
}


/*
 * The row that one destination row takes its pixels from: the source row
 * (and, for a filtered row, the one below it and the weight of it).
 */
static const UBYTE *source_row(XferState *s, const UBYTE **next, LONG *fy)
{
    const XferJob *j = s->job;
    LONG sy = s->y.pos;
    const UBYTE *row;

    *fy = 0;
    if (s->filtered)
    {
        if (sy < 0)
            sy = 0;
        if (sy > (LONG)(j->sh - 1) * 256)
            sy = (LONG)(j->sh - 1) * 256;
        *fy = sy & 255;
        row = j->src + (sy >> 8) * j->pitch;
        *next = *fy ? row + j->pitch : row;
        return row;
    }
    *next = NULL;
    return j->src + sy * j->pitch;
}


#if defined(__m68k__)
#define STORE16(out, v)     (*(UWORD_ALIAS *)(out) = (UWORD)(v))
#else
#define STORE16(out, v)     do { (out)[0] = (UBYTE)((v) >> 8); (out)[1] = (UBYTE)(v); } while (0)
#endif


#if CONF_WITH_VDI_ALPHA
#if defined(__m68k__)
#define LOAD16(p)           (*(const UWORD_ALIAS *)(p))
#else
#define LOAD16(p)           ((UWORD)(((UWORD)(p)[0] << 8) | (p)[1]))
#endif

/*
 * T_BLEND: count pixels mixed into the 16-bit words at 'out', each by the
 * alpha of its nearest source pixel times the weights.  A pixel of alpha 0
 * is neither read nor written.  A 1:1 row of a source with alpha is the
 * row of vdi_alpha.c; anything else goes pixel by pixel, with kc worked out
 * again only when the alpha changes (sources without alpha keep 255).
 */
static void blend_row(const XferJob *j, XferAxis *x, const UBYTE *row, UBYTE *out)
{
    WORD kind = j->kind & XK_MASK;
    LONG n = j->count, pbytes = xfer_kind_bits(j->kind) / 8;
    BOOL full = j->wp[0] >= VDI_ALPHA_FULL && j->wp[1] >= VDI_ALPHA_FULL && j->wp[2] >= VDI_ALPHA_FULL;
    BOOL opaque = FALSE;
    const UBYTE *p;
    ULONG rgb;
    UWORD a, last = 0, kr = 0, kg = 0, kb = 0, v;

    if ((kind == XK_ARGB32 || kind == XK_RGBA32) && j->sw == j->dw)
    {
        vdi_alpha_row565(out, row + x->pos * 4, n, kind == XK_ARGB32, j->wp);
        return;
    }
    while (n-- > 0)
    {
        p = row + x->pos * pbytes;
        a = xfer_alpha(p, j->kind);
        if (a)
        {
            if (a != last)
            {
                last = a;
                if (full)
                    kr = kg = kb = a + (a >> 7);
                else
                {
                    kr = vdi_alpha_k(a, j->wp[0]);
                    kg = vdi_alpha_k(a, j->wp[1]);
                    kb = vdi_alpha_k(a, j->wp[2]);
                }
                opaque = kr == 256 && kg == 256 && kb == 256;
            }
            rgb = xfer_unpack(p, j->kind);
            if (opaque)
                v = xfer_pack565(rgb);  /* mode 32's pixel, the one below not read */
            else
                v = vdi_alpha_mix565(LOAD16(out), rgb, kr, kg, kb);
            STORE16(out, v);
        }
        out += 2;
        AXIS_NEXT(*x);
    }
}
#endif


/*
 * make one row of count pixels at 'out': 16-bit words, or bytes of pixel
 * values with 'mask' (transparent sources onto planes)
 */
static void make_row(XferState *s, UBYTE *out, UBYTE *mask)
{
    const XferJob *j = s->job;
    WORD kind = j->kind & XK_MASK;
    BOOL d16 = IS_D16(j->planes);
    const UBYTE *next, *row;
    LONG fy, n = j->count, pbytes = xfer_kind_bits(j->kind) / 8;
    XferAxis x = s->x0;
    ULONG v;

    row = source_row(s, &next, &fy);
    if (kind == XK_1)
    {
        BOOL tr = j->transparent;
        LONG b;
        BOOL set;

        while (n-- > 0)
        {
            b = j->bit + x.pos;
            set = (row[b >> 3] >> (7 - (b & 7))) & 1;
            if (!set && tr)
            {
                if (mask)
                    *mask = 0;
            }
            else
            {
                v = set ? j->fg : j->bg;
                if (d16)
                    STORE16(out, v);
                else
                    *out = (UBYTE)v;
                if (mask)
                    *mask = 1;
            }
            out += d16 ? 2 : 1;
            if (mask)
                mask++;
            AXIS_NEXT(x);
        }
    }
    else if (kind == XK_8)
    {
        if (d16)
        {
            while (n-- > 0)
            {
                v = j->map[row[x.pos]];
                STORE16(out, v);
                out += 2;
                AXIS_NEXT(x);
            }
        }
        else
        {
            while (n-- > 0)
            {
                *out++ = (UBYTE)pen_for_index(j, row[x.pos]);
                AXIS_NEXT(x);
            }
        }
    }
    else if (s->filtered)
    {
        while (n-- > 0)
        {
            v = xfer_pack565(interpolate(row, next, x.pos, (LONG)j->sw, fy, j->kind, pbytes));
            STORE16(out, v);
            out += 2;
            AXIS_NEXT(x);
        }
    }
#if CONF_WITH_VDI_ALPHA
    else if (d16 && j->blend)
        blend_row(j, &x, row, out);
#endif
    else if (d16 && kind == XK_16)
    {
        /* 5-6-5 as it is: the green keeps its sixth bit (the reversed form is swapped) */
        const UBYTE *p;

        while (n-- > 0)
        {
            p = row + x.pos * 2;
            if (j->kind & XK_REV)
            {
                out[0] = p[1];
                out[1] = p[0];
            }
            else
            {
                out[0] = p[0];
                out[1] = p[1];
            }
            out += 2;
            AXIS_NEXT(x);
        }
    }
    else if (d16)
    {
        while (n-- > 0)
        {
            v = xfer_pack565(xfer_unpack(row + x.pos * pbytes, j->kind));
            STORE16(out, v);
            out += 2;
            AXIS_NEXT(x);
        }
    }
    else
    {
        while (n-- > 0)
        {
            *out++ = (UBYTE)pen_for(s, xfer_unpack(row + x.pos * pbytes, j->kind));
            AXIS_NEXT(x);
        }
    }
}


#if CONF_WITH_VDI_16BIT
void xfer_row16(XferState *s, UBYTE *dst, const UBYTE *prev)
{
    const XferJob *j = s->job;
    LONG i, bytes = j->count * 2;
    BOOL same = !s->filtered && !j->transparent && prev && s->y.pos == s->prev_y;

#if CONF_WITH_VDI_ALPHA
    if (j->blend)
        same = FALSE;                   /* each row is blended into its own pixels */
#endif
    if (same)
    {
        for (i = 0; i < bytes; i++)
            dst[i] = prev[i];
    }
    else
        make_row(s, dst, NULL);
    s->prev_y = s->y.pos;
    xfer_axis_next(&s->y);
}
#endif


void xfer_row_pix(XferState *s, UBYTE *pix, UBYTE *mask)
{
    const XferJob *j = s->job;

    if (j->transparent || s->prev_y != s->y.pos)
        make_row(s, pix, mask);
    s->prev_y = s->y.pos;
    xfer_axis_next(&s->y);
}


/*
 * The index path
 */

UBYTE xfer_itab_index(const UBYTE *map, WORD bits, ULONG rgb)
{
    ULONG mask = (1UL << bits) - 1;
    ULONG r = (rgb >> (24 - 3 * bits)) & (mask << (2 * bits));
    ULONG g = (rgb >> (16 - 2 * bits)) & (mask << bits);
    ULONG b = (rgb >> (8 - bits)) & mask;

    return map[r | g | b];
}


void xfer_itab_fill(const COLOR_ENTRY *colors, WORD count, WORD bits, UBYTE *map, void *work)
{
    ULONG *sq = (ULONG *)work;          /* sq[d + 255] = d * d, d -255 to 255 */
    ULONG *dr = sq + 511;               /* the red part of the distance of each colour */
    ULONG *drg = dr + count;            /* and with the green part */
    UBYTE *red = (UBYTE *)(drg + count), *green = red + count, *blue = green + count;
    WORD shift = 8 - bits, n = (WORD)(1 << bits), r, g, b, i;
    LONG d, half = 1L << (shift - 1), c;
    const ULONG *bsq;
    ULONG best, dist;
    UBYTE index;

    for (d = -255; d <= 255; d++)
        sq[d + 255] = (ULONG)(d * d);
    for (i = 0; i < count; i++)
    {
        red[i] = (UBYTE)(colors[i].rgb.red >> 8);
        green[i] = (UBYTE)(colors[i].rgb.green >> 8);
        blue[i] = (UBYTE)(colors[i].rgb.blue >> 8);
    }

    /*
     * The distance is the sum of three squares: the red and green parts are
     * worked out once per row of cells, and the squares come from a table.
     * The colours are tried in order and only a nearer one replaces the
     * best, so the first of equals wins.
     */
    for (r = 0; r < n; r++)
    {
        c = ((LONG)r << shift) + half + 255;
        for (i = 0; i < count; i++)
            dr[i] = sq[c - red[i]];
        for (g = 0; g < n; g++)
        {
            c = ((LONG)g << shift) + half + 255;
            for (i = 0; i < count; i++)
                drg[i] = dr[i] + sq[c - green[i]];
            for (b = 0; b < n; b++)
            {
                bsq = sq + ((LONG)b << shift) + half + 255;
                best = 0xffffffffUL;
                index = 0;
                for (i = 0; i < count; i++)
                {
                    dist = drg[i] + bsq[-(LONG)blue[i]];
                    if (dist < best)
                    {
                        best = dist;
                        index = (UBYTE)i;
                    }
                }
                *map++ = index;
            }
        }
    }
}


static BOOL same_rgb(const COLOR_RGB *a, const COLOR_RGB *b)
{
    return a->red == b->red && a->green == b->green && a->blue == b->blue;
}


#define RGB8(c)     (((ULONG)((c)->red >> 8) << 16) | ((ULONG)((c)->green >> 8) << 8) | ((c)->blue >> 8))

WORD xfer_remap(const COLOR_ENTRY *src, WORD scount, const COLOR_ENTRY *dst, WORD dcount,
                const UBYTE *map, WORD bits, UBYTE *remap, ULONG *spal)
{
    WORD i, j, flags = XFER_REMAP_EXACT | XFER_REMAP_SAME;
    const COLOR_RGB *c;

    /* remap and spal hold 256 entries, whatever the caller was told */
    if (scount > 256)
        scount = 256;
    if (dcount > 256)
        dcount = 256;
    for (i = 0; i < scount; i++)
    {
        c = &src[i].rgb;
        spal[i] = RGB8(c);
        /* the same index, or the first that has the colour (duplicates keep their index) */
        if (i < dcount && same_rgb(c, &dst[i].rgb))
            j = i;
        else
        {
            for (j = 0; j < dcount; j++)
                if (same_rgb(c, &dst[j].rgb))
                    break;
        }
        if (j == dcount)
        {
            flags &= ~XFER_REMAP_EXACT;
            j = xfer_itab_index(map, bits, spal[i]);
        }
        remap[i] = (UBYTE)j;
        if (j != i)
            flags &= ~XFER_REMAP_SAME;
    }
    return flags;
}


/* the four-sample filter over the colours of an 8-bit source */
static ULONG interpolate_index(const UBYTE *row0, const UBYTE *row1, LONG x, LONG width,
                               LONG fy, const ULONG *pal)
{
    LONG fx, right;

    if (x < 0)
        x = 0;
    if (x > (width - 1) * 256)
        x = (width - 1) * 256;
    fx = x & 255;
    x >>= 8;
    right = x + (fx != 0);
    return blend_rgb(pal[row0[x]], pal[row0[right]], pal[row1[x]], pal[row1[right]], fx, fy);
}


/* one row of the index path; with 'out' NULL only the error runs (rows above the part drawn) */
static void index_row(XferState *s, UBYTE *out)
{
    const XferJob *j = s->job;
    WORD kind = j->kind & XK_MASK;
    const UBYTE *next, *row;
    LONG fy, pbytes = xfer_kind_bits(j->kind) / 8, n, col, first, end;
    XferAxis x = s->x0;
    ULONG rgb;

    row = source_row(s, &next, &fy);
    if (!j->dither)
    {
        n = j->count;
        if (kind == XK_8)
        {
            while (n-- > 0)
            {
                *out++ = j->remap[row[x.pos]];
                AXIS_NEXT(x);
            }
        }
        else
        {
            while (n-- > 0)
            {
                rgb = s->filtered ? interpolate(row, next, x.pos, (LONG)j->sw, fy, j->kind, pbytes) :
                                    xfer_unpack(row + x.pos * pbytes, j->kind);
                *out++ = xfer_itab_index(j->imap, j->ibits, rgb);
                AXIS_NEXT(x);
            }
        }
        return;
    }

    /*
     * Floyd-Steinberg, left to right, as fVDI does it: err holds three
     * components a pixel, errors in sixteenths (at most 16 * 255, so words).
     * The old error at a column is used before its slot becomes the next
     * row's; the term down and to the right of the pixel before waits in
     * 'diagonal', the one to the right in 'carry'.
     */
    {
        WORD *err = j->err;
        LONG carry[3], diagonal[3], channel[3], value, e;
        ULONG adjusted;
        const COLOR_RGB *chosen;
        UBYTE pixel;
        WORD k, shift;

        first = (LONG)j->skipx;
        end = first + j->count;
        for (k = 0; k < 3; k++)
            carry[k] = diagonal[k] = 0;
        for (col = 0; col < (LONG)j->dw; col++)
        {
            if (kind == XK_8)
                rgb = s->filtered ? interpolate_index(row, next, x.pos, (LONG)j->sw, fy, j->spal) :
                                    j->spal[row[x.pos]];
            else
                rgb = s->filtered ? interpolate(row, next, x.pos, (LONG)j->sw, fy, j->kind, pbytes) :
                                    xfer_unpack(row + x.pos * pbytes, j->kind);
            adjusted = 0;
            for (k = 0, shift = 16; k < 3; k++, shift -= 8)
            {
                value = (LONG)((rgb >> shift) & 255) + ((err[k] + carry[k] + 8) >> 4);
                if (value < 0)
                    value = 0;
                else if (value > 255)
                    value = 255;
                channel[k] = value;
                adjusted |= (ULONG)value << shift;
            }
            pixel = xfer_itab_index(j->imap, j->ibits, adjusted);
            chosen = &j->dcol[pixel].rgb;
            channel[0] -= chosen->red >> 8;
            channel[1] -= chosen->green >> 8;
            channel[2] -= chosen->blue >> 8;
            for (k = 0; k < 3; k++)
            {
                e = channel[k];
                if (col)
                    err[k - 3] += (WORD)(e * 3);
                err[k] = (WORD)(e * 5 + diagonal[k]);
                diagonal[k] = e;
                carry[k] = e * 7;
            }
            err += 3;
            if (out && col >= first && col < end)
                out[col - first] = pixel;
            AXIS_NEXT(x);
        }
    }
}


void xfer_row_index(XferState *s, UBYTE *out)
{
    for (; s->dskip; s->dskip--)
    {
        index_row(s, NULL);
        xfer_axis_next(&s->y);
    }
    index_row(s, out);
    s->prev_y = s->y.pos;
    xfer_axis_next(&s->y);
}


void xfer_store_planar(UBYTE *line, WORD planes, LONG x0, LONG count,
                       const UBYTE *pix, const UBYTE *mask)
{
    LONG x = x0, end = x0 + count, k = 0;
    UWORD set[8];
    UWORD touch, old, bit, v;
    WORD first, last, i, p;
    UBYTE *w;

    while (x < end)
    {
        first = (WORD)(x & 15);
        last = 15;
        if (((end - 1) >> 4) == (x >> 4))
            last = (WORD)((end - 1) & 15);
        w = line + (x >> 4) * 2 * planes;
        touch = 0;
        for (p = 0; p < planes; p++)
            set[p] = 0;
        for (i = first; i <= last; i++, k++)
        {
            if (mask && !mask[k])
                continue;
            bit = (UWORD)(0x8000U >> i);
            touch |= bit;
            v = pix[k];
            for (p = 0; p < planes; p++, v >>= 1)
                if (v & 1)
                    set[p] |= bit;
        }
        if (touch)
        {
            for (p = 0; p < planes; p++)
            {
                old = (UWORD)(((UWORD)w[2 * p] << 8) | w[2 * p + 1]);
                old = (UWORD)((old & ~touch) | set[p]);
                w[2 * p] = (UBYTE)(old >> 8);
                w[2 * p + 1] = (UBYTE)old;
            }
        }
        x += last - first + 1;
    }
}
