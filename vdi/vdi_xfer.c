/*
 * vdi_xfer.c - vr_transfer_bits (170) and the scaled vro_cpyfm and vrt_cpyfm
 *
 * vr_transfer_bits copies a rectangle of a bitmap of one pixel format onto a
 * rectangle of another, scaling it.  This is the slice that EmuTOS has:
 *
 *   sources:      1 bit, 8-bit indexed, 15 and 16 bits (also byte-swapped),
 *                 24 bits (also reversed) and 32 bits (xRGB)
 *   destinations: the workstation (the screen, or the bitmap of a bitmap
 *                 workstation), memory in the format of the screen, or
 *                 memory of 8-bit indices (not from 1-bit sources)
 *   modes:        0 (copy) and 32 (replace) for all sources, 33 (transparent)
 *                 for 1-bit ones; with bit 7 (128), error diffusion onto
 *                 8-bit indices and planes
 *
 * What is not done is not advertised: bits 0 and 1 of work_out[30] stay
 * clear.  The pixel conversion is in vdi_xfercore.c.
 *
 * A 16-bit destination takes its pixels in 5-6-5, the green keeps its sixth
 * bit; bit 7 has no effect there.  Planes take the pen nearest the colour of
 * the source pixel (in the workstation's colours), asked once per colour that
 * is not one of the last few seen.
 *
 * 8-bit indices, and planes with bit 7, are worked out as fVDI does: through
 * the inverse table of the destination palette (vdi_ctab.c), which for planes
 * is the screen's colours by pixel value; reductions of direct colours (and
 * of dithered 8-bit sources) are filtered, and a source whose colours are all
 * in the destination palette keeps its indices and is not dithered.  The
 * palette of an 8-bit bitmap that has no table is the workstation's pens, by
 * pen number, both as source and as destination; it must then have no
 * inverse table or one made for those colours.  A destination with a table
 * needs the inverse table made for it (v_create_itab).  8-bit indices with
 * neither table nor dithering, at the same size, are copied as they are.
 *
 * vro_cpyfm and vrt_cpyfm with bit 15 of the mode set (a VDI call only) scale
 * the source rectangle onto the destination rectangle by nearest neighbour,
 * one row at a time through the ordinary copy.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "lineavars.h"
#include "tosvars.h"
#include "string.h"
#include "vdi_kept.h"
#include "vdi_nvdimath.h"
#include "vdi_xfercore.h"

#if CONF_WITH_VDI_NVDI_EXT

/* a pointer out of two words of CONTRL: the words need not be aligned for one */
static void *ptr_at(const WORD *p)
{
    return (void *)(((ULONG)(UWORD)p[0] << 16) | (ULONG)(UWORD)p[1]);
}


/* a pointer into two words of CONTRL */
static void put_ptr(WORD *p, const void *v)
{
    p[0] = (WORD)((ULONG)v >> 16);
    p[1] = (WORD)(ULONG)v;
}


/* the planes of the destination: XD_16, 1, 2, 4 or 8, or 0 if the screen is none of those */
static WORD screen_planes(void)
{
#if CONF_WITH_VDI_16BIT
    if (v_planes == 16)
        return XD_16;
#endif
    if (v_planes == 1 || v_planes == 2 || v_planes == 4 || v_planes == 8)
        return (WORD)v_planes;
    return 0;
}


/* the VDI pen a colour is nearest to, as the pixel value of the planes */
typedef struct
{
    Vwk *vwk;
    const COLOR_TAB *ctab;      /* the colour table of the source, NULL: the workstation's pens */
    LONG ncol;                  /* its colours, as read once (1-256) */
} XferCtx;


static ULONG pixel_of_colour(Vwk *vwk, const COLOR_RGB *c)
{
    return (ULONG)MAP_COL[vdi_nearest_pen_in(vwk, c)];
}


static ULONG pen_of_rgb(void *ctx, ULONG rgb)
{
    COLOR_RGB c;

    c.reserved = 0;
    c.red = (UWORD)(((rgb >> 16) & 255) * 257UL);
    c.green = (UWORD)(((rgb >> 8) & 255) * 257UL);
    c.blue = (UWORD)((rgb & 255) * 257UL);
    return pixel_of_colour(((XferCtx *)ctx)->vwk, &c);
}


/*
 * the colour of an index of an 8-bit source: the table's, or the pen's; an
 * index beyond the table (the source changed since it was checked) is black
 */
static void index_colour(const XferCtx *x, ULONG index, COLOR_RGB *c)
{
    if (!x->ctab)
        vdi_pen_rgb(x->vwk, (WORD)index, c);
    else if (index < (ULONG)x->ncol)
        *c = x->ctab->colors[index].rgb;
    else
    {
        c->reserved = 0;
        c->red = c->green = c->blue = 0;
    }
}


static ULONG pen_of_index(void *ctx, ULONG index)
{
    COLOR_RGB c;

    index_colour((XferCtx *)ctx, index, &c);
    return pixel_of_colour(((XferCtx *)ctx)->vwk, &c);
}


/* the pixel of the raster foreground (bg 0) or background (bg 1) in the destination format */
static UWORD raster_pixel(Vwk *vwk, WORD planes, WORD bg)
{
    const VwkColors *col = &vwk->col;

#if CONF_WITH_VDI_16BIT
    if (planes == XD_16)
    {
        const COLOR_RGB *c = &col->req[VDI_COL_RASTER][bg];

        if (col->rgbmask & ((UWORD)1 << (2 * VDI_COL_RASTER + bg)))
            return vdi_pack16(c->red, c->green, c->blue);
        return vwk->ext->palette[MAP_COL[col->pen[VDI_COL_RASTER][bg]]];
    }
#endif
    return (UWORD)MAP_COL[col->pen[VDI_COL_RASTER][bg]];
}


/*
 * The tables of the index path, found once the memory is had: nothing
 * sleeps between finding them and the end of the call
 */
typedef struct
{
    const VdiItab *itab;        /* the destination palette and its inverse map */
    const COLOR_ENTRY *scol;    /* XK_8: the colours of the source */
    WORD scount;
} XferTables;


/*
 * Get the tables: 'ckind' is the snapshot of the workstation that is the
 * destination palette (-1: the bitmap's own table 'dctab', with the inverse
 * table 'ditab'), 'inverse' whether its inverse table is the VDI's; 'pens'
 * whether an 8-bit source takes the colours of the pens, else 'sctab' with
 * 'scount' colours (checked by the caller).  FALSE if one cannot be had.
 */
static BOOL index_tables(Vwk *vwk, const COLOR_TAB *sctab, WORD scount,
                         const COLOR_TAB *dctab, ULONG ditab,
                         WORD ckind, BOOL inverse, BOOL pens, XferTables *t)
{
    const VdiCtab *node;

    t->itab = NULL;
    t->scol = NULL;
    t->scount = 0;

    /* these may sleep */
    if (ckind >= 0 && !vdi_ctab_prepare(vwk, ckind, inverse))
        return FALSE;
    if (pens && !vdi_ctab_prepare(vwk, VDI_CTAB_PENS, FALSE))
        return FALSE;

    /* these do not */
    if (ckind < 0)
        t->itab = vdi_itab_find(vwk, ditab, dctab);
    else
    {
        node = vdi_ctab_get(vwk, ckind, inverse);
        if (!node)
            return FALSE;
        t->itab = inverse ? node->inverse : vdi_itab_find(vwk, ditab, &node->table);
    }
    if (!t->itab)
        return FALSE;
    if (pens)
    {
        node = vdi_ctab_get(vwk, VDI_CTAB_PENS, FALSE);
        if (!node)
            return FALSE;
        t->scol = node->table.colors;
        t->scount = (WORD)node->table.no_colors;
    }
    else if (sctab)
    {
        t->scol = sctab->colors;
        t->scount = scount;
    }
    return TRUE;
}


/*
 * vr_transfer_bits
 *
 * intin[0]: the mode; CONTRL[7..8]: the GCBITMAP of the source; CONTRL[9..10]:
 * the one of the destination, or 0 for the workstation; ptsin: the source
 * rectangle, then the destination rectangle (x1, y1, x2, y2, both ends
 * inclusive).  Nothing is drawn if the call is not understood or the memory
 * is not usable (see the refusals in vdi_xfercore.h); nothing is returned.
 *
 * The two GCBITMAPs and the header of the source's colour table are read
 * once, into copies: what is checked is what is used, whatever the program
 * (another of its threads, or another program sharing the memory) writes
 * there meanwhile, while this call sleeps in an allocation or not.
 */
void vdi_vr_transfer_bits(Vwk *vwk)
{
    const GCBITMAP *src, *dst;
    GCBITMAP sbm, dbm;
    COLOR_TAB_HEAD shead;
    const COLOR_TAB *sctab;
    RECT16 sr, dr;
    LONG cx1, cy1, cx2, cy2;    /* the destination, where it may be drawn */
    LONG dx1, dy1, dx2, dy2;    /* the part of the destination rectangle that is */
    LONG pitch, xbase, ybase, x, y, bytes, sx, mapbytes, rowbytes, palbytes, errbytes;
    UBYTE *base, *block, *line, *row;
    ULONG ssize, dsize;
    UWORD mode;
    WORD kind, planes, dkind, k, ckind = -1;
    BOOL transparent = FALSE, dither, index, inverse = FALSE, pens = FALSE;
    XferJob job;
    XferState st;
    XferCtx ctx;
    XferTables tables;
    LONG ncol = 0;

    if (CONTRL[3] < 1 || CONTRL[1] < 4)
        return;
    src = (const GCBITMAP *)ptr_at(CONTRL + 7);
    dst = (const GCBITMAP *)ptr_at(CONTRL + 9);
    if (!src)
        return;
    sbm = *src;
    src = &sbm;
    if (dst)
    {
        dbm = *dst;
        dst = &dbm;
    }
    sctab = sbm.ctab;
    sr.x1 = PTSIN[0];
    sr.y1 = PTSIN[1];
    sr.x2 = PTSIN[2];
    sr.y2 = PTSIN[3];
    dr.x1 = PTSIN[4];
    dr.y1 = PTSIN[5];
    dr.x2 = PTSIN[6];
    dr.y2 = PTSIN[7];

    /* the mode; bit 7 asks for dithering, where there are 256 colours or fewer */
    mode = (UWORD)INTIN[0];
    dither = (mode & 0x80) != 0;
    mode &= ~0x80U;
    kind = xfer_kind(src->px_format);
    k = kind & XK_MASK;
    if (k == XK_NONE)
        return;
    if (mode == 33)
    {
        if (k != XK_1)
            return;
        transparent = TRUE;
    }
    else if (mode != 0 && mode != 32)
        return;

    ssize = xfer_src_size(src, kind);
    if (ssize == 0 || !xfer_rect_in(src, &sr))
        return;
    if (dr.x1 > dr.x2 || dr.y1 > dr.y2)
        return;

    planes = screen_planes();
    if (planes == 0)
        return;
    if (dst)
    {
        /*
         * memory in the format of the screen, or of 8-bit indices: whatever is
         * clipped there, no clipping
         */
        if (dst->px_format == XF_PX_8 && dst->bits == 8)
            dkind = XD_INDEX8;
        else if (dst->px_format == vdi_px_format())
            dkind = planes;
        else
            return;
        dsize = xfer_dst_size(dst, dkind);
        if (dsize == 0 || !xfer_rect_in(dst, &dr) || dst->addr == v_bas_ad)
            return;                 /* the screen is the workstation, not memory */
        base = dst->addr;
        pitch = dst->width;
        xbase = dst->xmin;
        ybase = dst->ymin;
        cx1 = dr.x1;
        cy1 = dr.y1;
        cx2 = dr.x2;
        cy2 = dr.y2;
    }
    else
    {
        /* this workstation: the screen, or the bitmap while a bitmap workstation is drawn in */
        dkind = planes;
        base = v_bas_ad;
        pitch = (LONG)v_lin_wr;
        xbase = ybase = 0;
        dsize = (ULONG)pitch * (ULONG)((LONG)yres + 1);
        cx1 = cy1 = 0;
        cx2 = xres;
        cy2 = yres;
        if (vwk->clip)
        {
            if (vwk->xmn_clip > cx1)
                cx1 = vwk->xmn_clip;
            if (vwk->ymn_clip > cy1)
                cy1 = vwk->ymn_clip;
            if (vwk->xmx_clip < cx2)
                cx2 = vwk->xmx_clip;
            if (vwk->ymx_clip < cy2)
                cy2 = vwk->ymx_clip;
        }
    }
    if (xfer_overlap(src->addr, ssize, base, dsize))
        return;

    /*
     * The pixels are indices of a palette, worked out as fVDI does: onto 8-bit
     * indices, and dithered onto planes.  Onto 16 bits dithering has no
     * effect; a 1-bit source has its two colours.
     */
    index = (dkind == XD_INDEX8) || (dither && dkind != XD_16 && k != XK_1);
    if (dkind == XD_INDEX8 && k == XK_1)
        return;
    if (!index)
        dither = FALSE;

    dx1 = dr.x1 > cx1 ? dr.x1 : cx1;
    dy1 = dr.y1 > cy1 ? dr.y1 : cy1;
    dx2 = dr.x2 < cx2 ? dr.x2 : cx2;
    dy2 = dr.y2 < cy2 ? dr.y2 : cy2;
    if (dx1 > dx2 || dy1 > dy2)
        return;

    /* the job */
    memset(&job, 0, sizeof(job));
    job.kind = kind;
    job.planes = dkind;
    job.transparent = transparent;
    job.pitch = src->width;
    job.sw = (ULONG)((LONG)sr.x2 - sr.x1 + 1);
    job.sh = (ULONG)((LONG)sr.y2 - sr.y1 + 1);
    job.dw = (ULONG)((LONG)dr.x2 - dr.x1 + 1);
    job.dh = (ULONG)((LONG)dr.y2 - dr.y1 + 1);
    job.skipx = (ULONG)(dx1 - dr.x1);
    job.skipy = (ULONG)(dy1 - dr.y1);
    job.count = dx2 - dx1 + 1;
    job.rows = dy2 - dy1 + 1;
    sx = (LONG)sr.x1 - src->xmin;
    job.src = src->addr + ((LONG)sr.y1 - src->ymin) * src->width;
    if (k == XK_1)
    {
        job.src += sx >> 3;
        job.bit = (WORD)(sx & 7);
    }
    else
        job.src += sx * (xfer_kind_bits(kind) / 8);
    ctx.vwk = vwk;
    ctx.ctab = NULL;
    ctx.ncol = 0;
    job.ctx = &ctx;
    job.pen_rgb = pen_of_rgb;
    job.pen_index = pen_of_index;

    if (index && dkind == XD_INDEX8 && k == XK_8 && !dither && !src->ctab && !dst->ctab &&
        !src->itab && !dst->itab && job.sw == job.dw && job.sh == job.dh)
    {
        /* no colour tables, the same size: the bytes as they are, as fVDI */
        row = (UBYTE *)job.src;
        line = base + (dy1 - ybase) * pitch + (dx1 - xbase);
        for (y = 0; y < job.rows; y++, row += job.pitch, line += pitch)
            memcpy(line, row, job.count);
        return;
    }

    if (k == XK_1)
    {
        job.fg = raster_pixel(vwk, planes, 0);
        job.bg = raster_pixel(vwk, planes, 1);
    }
    else if (k == XK_8)
    {
        /*
         * the colours of the indices: the table of the source (its header
         * read once; the index path wants a sound one, the others only a
         * count of 1-256) or, without one, the pens of the workstation by pen
         * number (as since the first vr_transfer_bits, though vq_ctab lists
         * the colours by pixel value)
         */
        if (vdi_ctab_head(sctab, &shead) ||
            (sctab && !index && shead.no_colors >= 1 && shead.no_colors <= 256))
        {
            ctx.ctab = sctab;
            ncol = shead.no_colors;
        }
        else if (sctab && index)
            return;                 /* the index path takes only a sound table */
        else
        {
            sctab = NULL;           /* an unusable table: the pens, as before */
            ncol = numcolors > 256 ? 256 : numcolors;
        }
        ctx.ncol = ncol;
        if (ncol < 1)
            return;
        if (ncol < 256)
        {
            /* an index beyond the table refuses the whole call */
            for (y = 0; y < (LONG)job.sh; y++)
            {
                row = (UBYTE *)job.src + y * job.pitch;
                for (x = 0; x < (LONG)job.sw; x++)
                    if (row[x] >= ncol)
                        return;
            }
        }
    }

    if (index)
    {
        /* the destination palette: the bitmap's table, the pens, or the screen's colours */
        if (dkind == XD_INDEX8)
        {
            if (!dst->ctab)
            {
                ckind = VDI_CTAB_PENS;
                inverse = !dst->itab;
            }
        }
        else
        {
            ckind = VDI_CTAB_CURRENT;
            inverse = TRUE;
        }
        pens = (k == XK_8 && !sctab);
    }

    /*
     * The allocation below can sleep.  base, pitch and the clip bounds were
     * read before it: under MiNT a bitmap workstation that another program
     * closes or resizes meanwhile leaves them stale.  Not guarded: the same
     * known limit as the bitmap swap's (see vdi_bitmap.c and vdi_sleep.h).
     */
    /*
     * work memory: the pixel of each index, a row of pixel values and, if
     * transparent, a mask; the index path: the colours of the source, the
     * error of the dithering and the index of each source index
     */
    mapbytes = (!index && k == XK_8) ? 512L : 0L;
    palbytes = (index && k == XK_8) ? 1024L + 256L : 0L;
    errbytes = dither ? 6L * (LONG)job.dw : 0L;
    rowbytes = (dkind != XD_16 && dkind != XD_INDEX8) ? job.count : 0L;
    bytes = mapbytes + palbytes + errbytes + rowbytes * (transparent ? 2 : 1);
    block = NULL;
    if (bytes)
    {
        block = (UBYTE *)vdi_alloc_kept(bytes);
        if (!block)
            return;
    }

    if (index)
    {
        ULONG *spal = (ULONG *)block;
        UBYTE *remap = block + 1024;

        /* indices beyond the source's table (a source changed since it was checked) map to 0 */
        if (palbytes)
            memset(block, 0, palbytes);

        /* the tables; nothing sleeps from here on */
        if (!index_tables(vwk, sctab, (WORD)ncol, dst ? dst->ctab : NULL, dst ? (ULONG)dst->itab : 0UL,
                          ckind, inverse, pens, &tables))
            goto done;
        job.index = TRUE;
        job.imap = VDI_ITAB_MAP(tables.itab);
        job.ibits = tables.itab->bits;
        job.dcol = tables.itab->colors;
        if (k == XK_8)
        {
            /* a source with only colours of the destination is not dithered (fVDI) */
            if (xfer_remap(tables.scol, tables.scount, tables.itab->colors, tables.itab->count,
                           job.imap, job.ibits, remap, spal) & XFER_REMAP_EXACT)
                dither = FALSE;
            job.spal = spal;
            job.remap = remap;
        }
        if (dither)
        {
            job.dither = TRUE;
            job.err = (WORD *)(block + palbytes);
            memset(job.err, 0, errbytes);
        }
    }
    else if (k == XK_8)
    {
        job.map = (UWORD *)block;
        for (x = 0; x < 256; x++)
        {
            if (planes != XD_16)
                job.map[x] = 0xffff;       /* not asked yet */
            else if (x < ncol)
            {
                COLOR_RGB c;
                ULONG rgb;

                index_colour(&ctx, (ULONG)x, &c);
                rgb = ((ULONG)(c.red >> 8) << 16) | ((ULONG)(c.green >> 8) << 8) | (c.blue >> 8);
                job.map[x] = xfer_pack565(rgb);
            }
            else
                job.map[x] = 0;
        }
    }

    xfer_begin(&st, &job);
    if (index)
    {
        UBYTE *pix = block + palbytes + errbytes;

        if (dkind == XD_INDEX8)
        {
            line = base + (dy1 - ybase) * pitch + (dx1 - xbase);
            for (y = 0; y < job.rows; y++, line += pitch)
                xfer_row_index(&st, line);
        }
        else
        {
            line = base + (dy1 - ybase) * pitch;
            for (y = 0; y < job.rows; y++, line += pitch)
            {
                xfer_row_index(&st, pix);
                xfer_store_planar(line, planes, dx1 - xbase, job.count, pix, NULL);
            }
        }
    }
#if CONF_WITH_VDI_16BIT
    else if (planes == XD_16)
    {
        row = base + (dy1 - ybase) * pitch + (dx1 - xbase) * 2;
        for (y = 0; y < job.rows; y++, row += pitch)
            xfer_row16(&st, row, y ? row - pitch : NULL);
    }
#endif
    else
    {
        UBYTE *pix = block + mapbytes;
        UBYTE *mask = transparent ? pix + job.count : NULL;

        line = base + (dy1 - ybase) * pitch;
        for (y = 0; y < job.rows; y++, line += pitch)
        {
            xfer_row_pix(&st, pix, mask);
            xfer_store_planar(line, planes, dx1 - xbase, job.count, pix, mask);
        }
    }
done:
    if (block)
        vdi_free_kept(block);
}


/*
 * Scaled vro_cpyfm and vrt_cpyfm (bit 15 of the mode)
 *
 * The source rectangle is mapped onto the destination rectangle by nearest
 * neighbour with the rational stepping of the transfer above, a row at a
 * time into a row of the source's own format, and each of those rows goes
 * through the ordinary copy with the low bits of the mode.  A destination
 * that is the screen is clipped to the workstation's clipping rectangle
 * (when it is on) without moving the sampling; one in memory is not, but
 * must hold the rectangle.  The source must be memory in the format of the
 * screen (a standard form too), or monochrome for vrt_cpyfm.
 */

/* the same layout as the MFDB of vdi_raster.c */
typedef struct
{
    void *fd_addr;
    WORD fd_w;
    WORD fd_h;
    WORD fd_wdwidth;
    WORD fd_stand;
    WORD fd_nplanes;
    WORD fd_r1;
    WORD fd_r2;
    WORD fd_r3;
} SMFDB;


/* the pixel of a source MFDB (nearest source pixel, device or standard form) */
static ULONG mfdb_pixel(const SMFDB *b, LONG x, LONG y, BOOL packed)
{
    const UBYTE *base = (const UBYTE *)b->fd_addr;
    ULONG pixel = 0;
    LONG offset;
    WORD p;

    if (packed && !b->fd_stand && b->fd_nplanes > 1)
    {
        base += y * (LONG)b->fd_wdwidth * 2 * b->fd_nplanes + x * (b->fd_nplanes / 8);
        for (p = 0; p < b->fd_nplanes / 8; p++)
            pixel = (pixel << 8) | *base++;
        return pixel;
    }
    for (p = 0; p < b->fd_nplanes; p++)
    {
        if (b->fd_stand)
            offset = (((LONG)p * b->fd_h + y) * b->fd_wdwidth + (x >> 4)) * 2;
        else
            offset = ((y * b->fd_wdwidth + (x >> 4)) * b->fd_nplanes + p) * 2;
        if ((base[offset + ((x & 15) >> 3)] << (x & 7)) & 0x80)
            pixel |= 1UL << ((packed && b->fd_stand) ? 15 - p : p);   /* as vr_trnfm16: plane 0 is bit 15 */
    }
    return pixel;
}


/* put a pixel into a row of the device form of the screen's planes (the row is cleared) */
static void row_put(UBYTE *row, LONG x, WORD planes, BOOL packed, ULONG pixel)
{
    WORD p;

    if (packed && planes > 1)
    {
        row += x * (planes / 8);
        for (p = planes / 8; p-- > 0; )
        {
            row[p] = (UBYTE)pixel;
            pixel >>= 8;
        }
        return;
    }
    for (p = 0; p < planes; p++)
        if (pixel & (1UL << p))
            row[((x >> 4) * planes + p) * 2 + ((x & 15) >> 3)] |= (UBYTE)(0x80 >> (x & 7));
}


/* the size of a source or destination MFDB's memory in bytes, 0 if it cannot be used as one */
static ULONG mfdb_size(const SMFDB *b, WORD planes)
{
    LONG stride;

    if (!b->fd_addr || ((ULONG)b->fd_addr & 1UL) || b->fd_w <= 0 || b->fd_h <= 0 ||
        (LONG)b->fd_wdwidth < ((LONG)b->fd_w + 15) / 16 || b->fd_nplanes != planes ||
        (b->fd_stand != 0 && b->fd_stand != 1))
        return 0;
    stride = (LONG)b->fd_wdwidth * 2 * planes;
    if (stride > 0x7fffffffL / b->fd_h)
        return 0;
    return (ULONG)stride * (ULONG)b->fd_h;
}


/*
 * 'copy' is the ordinary copy of the caller (cpy_raster with its arguments);
 * the call has CONTRL, INTIN and PTSIN of a vro_cpyfm or vrt_cpyfm with
 * bit 15 of INTIN[0] set.
 */
void vdi_raster_scaled(void *raster, void *info, void (*copy)(void *, void *),
                       BOOL transparent, BOOL clip_on, const VwkClip *clip)
{
    WORD *old_contrl = CONTRL, *old_intin = INTIN, *old_ptsin = PTSIN;
    WORD contrl2[11], intin2[3], ptsin2[8], pts[8];
    SMFDB staged, scrmfdb, *src, *dst;
    UBYTE *buffer;
    XferAxis xinit, xa, ya;
    LONG xmin, ymin, xmax, ymax, sw, sh, dw, dh, x, width, row, prev, bytes, j;
    ULONG sbytes, dbytes;
    WORD planes, splanes, mode, i;
    BOOL packed, to_screen;

    if (CONTRL[1] < 4 || CONTRL[3] < (transparent ? 3 : 1))
        return;
    mode = (WORD)(INTIN[0] & 0x7fff);
    if (transparent ? (mode < 1 || mode > 4) : (mode > 31))
        return;
    planes = screen_planes();
    if (planes == 0)
        return;
    if (planes == XD_16)
        planes = 16;
    packed = planes > 8;
    splanes = transparent ? 1 : planes;
    for (i = 0; i < 8; i++)
        pts[i] = PTSIN[i];
    src = (SMFDB *)ptr_at(CONTRL + 7);
    dst = (SMFDB *)ptr_at(CONTRL + 9);
    if (!src)
        return;
    sbytes = mfdb_size(src, splanes);
    if (sbytes == 0 || pts[0] < 0 || pts[1] < 0 || pts[2] >= src->fd_w || pts[3] >= src->fd_h ||
        pts[2] < pts[0] || pts[3] < pts[1] || pts[6] < pts[4] || pts[7] < pts[5])
        return;

    to_screen = !dst || !dst->fd_addr;
    if (to_screen)
    {
        dbytes = (ULONG)v_lin_wr * (ULONG)((LONG)yres + 1);
        if (xfer_overlap(src->fd_addr, sbytes, v_bas_ad, dbytes))
            return;
        xmin = ymin = 0;
        xmax = xres;
        ymax = yres;
        if (clip_on)
        {
            if (clip->xmn_clip > xmin)
                xmin = clip->xmn_clip;
            if (clip->ymn_clip > ymin)
                ymin = clip->ymn_clip;
            if (clip->xmx_clip < xmax)
                xmax = clip->xmx_clip;
            if (clip->ymx_clip < ymax)
                ymax = clip->ymx_clip;
        }
        memset(&scrmfdb, 0, sizeof(scrmfdb));
        dst = &scrmfdb;
    }
    else
    {
        dbytes = mfdb_size(dst, planes);
        if (dbytes == 0 || dst->fd_stand || (UBYTE *)dst->fd_addr == v_bas_ad ||
            xfer_overlap(src->fd_addr, sbytes, dst->fd_addr, dbytes) ||
            pts[4] < 0 || pts[5] < 0 || pts[6] >= dst->fd_w || pts[7] >= dst->fd_h)
            return;
        xmin = ymin = 0;
        xmax = dst->fd_w - 1;
        ymax = dst->fd_h - 1;
    }
    if (pts[4] > xmin)
        xmin = pts[4];
    if (pts[5] > ymin)
        ymin = pts[5];
    if (pts[6] < xmax)
        xmax = pts[6];
    if (pts[7] < ymax)
        ymax = pts[7];
    if (xmin > xmax || ymin > ymax)
        return;

    sw = (LONG)pts[2] - pts[0] + 1;
    sh = (LONG)pts[3] - pts[1] + 1;
    dw = (LONG)pts[6] - pts[4] + 1;
    dh = (LONG)pts[7] - pts[5] + 1;
    width = xmax - xmin + 1;
    bytes = ((width + 15) / 16) * 2 * splanes;
    buffer = (UBYTE *)vdi_alloc_kept_blit(bytes);   /* the copy may use the blitter */
    if (!buffer)
        return;

    /* the call as a copy of one row: its own words for the parameters */
    for (i = 0; i < 11; i++)
        contrl2[i] = old_contrl[i];
    intin2[0] = mode;
    if (transparent)
    {
        intin2[1] = old_intin[1];
        intin2[2] = old_intin[2];
    }
    memset(&staged, 0, sizeof(staged));
    staged.fd_addr = buffer;
    staged.fd_w = (WORD)width;
    staged.fd_h = 1;
    staged.fd_wdwidth = (WORD)((width + 15) / 16);
    staged.fd_nplanes = splanes;
    put_ptr(contrl2 + 7, &staged);
    put_ptr(contrl2 + 9, dst);

    xfer_axis_start(&xinit, (ULONG)sw, (ULONG)dw, (ULONG)(xmin - pts[4]), FALSE);
    xfer_axis_start(&ya, (ULONG)sh, (ULONG)dh, (ULONG)(ymin - pts[5]), FALSE);
    prev = -1;
    for (row = ymin; row <= ymax; row++)
    {
        if (ya.pos != prev)
        {
            for (j = 0; j < bytes; j++)
                buffer[j] = 0;
            xa = xinit;
            for (x = 0; x < width; x++)
            {
                row_put(buffer, x, splanes, packed,
                        mfdb_pixel(src, xa.pos + pts[0], ya.pos + pts[1], packed));
                xfer_axis_next(&xa);
            }
        }
        ptsin2[0] = ptsin2[1] = ptsin2[3] = 0;
        ptsin2[2] = (WORD)(width - 1);
        ptsin2[4] = (WORD)xmin;
        ptsin2[6] = (WORD)xmax;
        ptsin2[5] = ptsin2[7] = (WORD)row;
        CONTRL = contrl2;
        INTIN = intin2;
        PTSIN = ptsin2;
        copy(raster, info);
        CONTRL = old_contrl;
        INTIN = old_intin;
        PTSIN = old_ptsin;
        prev = ya.pos;
        xfer_axis_next(&ya);
    }
    vdi_free_kept(buffer);
}

#endif /* CONF_WITH_VDI_NVDI_EXT */
