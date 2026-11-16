/*
 * vdi_xfercore.h - the pixel conversion of vr_transfer_bits
 *
 * Pure functions on plain buffers and rectangles: no hardware, no VDI
 * globals.  vdi_xfer.c resolves the workstation, clips, gets the work
 * buffers and calls them row by row; the host tests build vdi_xfercore.c
 * on its own.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_XFERCORE_H
#define VDI_XFERCORE_H

#include "portab.h"
#include "config.h"
#include "vdi_nvdiext.h"

/* the pixel formats (GCBITMAP.px_format) of the sources */
#define XF_PX_1         0x01020101UL    /* 1 bit, the leftmost pixel in the top bit */
#define XF_PX_8         0x01020808UL    /* 8-bit index into a colour table */
#define XF_PX_15        0x03420f10UL    /* 0rrrrrgggggbbbbb */
#define XF_PX_16        0x03021010UL    /* rrrrrggggggbbbbb */
#define XF_PX_24        0x03021818UL    /* bytes r, g, b */
#define XF_PX_32        0x03421820UL    /* bytes x, r, g, b; x is not read */
#define XF_PX_REVERSED  0x00800000UL    /* 15 and 16 bits: low byte first; 24 bits: b, g, r */

/* what xfer_kind() makes of a pixel format */
#define XK_NONE         0
#define XK_1            1
#define XK_8            2
#define XK_15           3
#define XK_16           4
#define XK_24           5
#define XK_32           6
#define XK_MASK         0x0f
#define XK_REV          0x10            /* or-ed in: PX_REVERSED */

/* the destinations (16-bit words only with CONF_WITH_VDI_16BIT): 16-bit words (red, green and blue in 5-6-5), or 1, 2, 4 and 8 planes */
#define XD_16           16

/* the source kind of a pixel format, XK_NONE if it is not one */
WORD xfer_kind(ULONG px_format);

/* the size of a pixel of a kind in bits */
WORD xfer_kind_bits(WORD kind);

/* a pixel of a direct kind (not XK_1 or XK_8) as 0x00rrggbb with 8-bit components */
ULONG xfer_unpack(const UBYTE *p, WORD kind);

/* 0x00rrggbb as the 16-bit pixel rrrrrggggggbbbbb (components cut) */
UWORD xfer_pack565(ULONG rgb);

/*
 * The size in bytes of the memory of a bitmap as the source of a kind, or
 * of a destination of 'planes' planes (XD_16, 1, 2, 4 or 8), 0 if it is
 * not usable: no address, a description that does not fit the format
 * (bits, line width, alignment of 2- and 4-byte pixels), an origin or a
 * size that RECT16 coordinates cannot address, or more than 2^31 - 1 bytes.
 */
ULONG xfer_src_size(const GCBITMAP *bm, WORD kind);
ULONG xfer_dst_size(const GCBITMAP *bm, WORD planes);

/* TRUE if the rectangle is not upside down and lies inside the bitmap (both inclusive) */
BOOL xfer_rect_in(const GCBITMAP *bm, const RECT16 *r);

/* TRUE if the byte ranges overlap */
BOOL xfer_overlap(const void *a, ULONG alen, const void *b, ULONG blen);

/* rational stepping over one axis: 'skip' pixels into the destination, 'filtered' in 1/256 pixels */
typedef struct
{
    LONG pos;
    ULONG error, step, remainder, denominator;
} XferAxis;

void xfer_axis_start(XferAxis *a, ULONG source, ULONG dest, ULONG skip, BOOL filtered);
void xfer_axis_next(XferAxis *a);

/*
 * A pixel value of the destination for a colour, asked per pixel of a
 * direct source onto planes ('value' is 0x00rrggbb) and, lazily, per
 * index of an 8-bit source ('value' is the index).  The answer is at most
 * 255.
 */
typedef ULONG (*XferPenFn)(void *ctx, ULONG value);

/*
 * Inverse colour maps (v_create_itab, the dithering): for each cell of RGB
 * at 'bits' bits a component (3, 4 or 5), the index of the palette colour
 * nearest the centre of the cell, at 8 bits a component and by squared
 * distance, the first of equals.  The map has XFER_ITAB_CELLS(bits) bytes,
 * the cell of red r, green g, blue b being (r << 2 * bits) | (g << bits) | b;
 * xfer_itab_fill() needs XFER_ITAB_WORK(count) bytes of work memory, LONG
 * aligned, for a palette of 'count' (1-256) colours.  It is an approximation
 * (the search is made once, at the centre of each cell), as fVDI's.
 */
#define XFER_ITAB_MIN_BITS      3
#define XFER_ITAB_MAX_BITS      5
#define XFER_ITAB_CELLS(bits)   (1L << (3 * (bits)))
#define XFER_ITAB_WORK(count)   (4L * 511 + 8L * (count) + 4L * (count))
void xfer_itab_fill(const COLOR_ENTRY *colors, WORD count, WORD bits, UBYTE *map, void *work);

/* the index a map gives a colour 0x00rrggbb */
UBYTE xfer_itab_index(const UBYTE *map, WORD bits, ULONG rgb);

typedef struct
{
    WORD kind;                  /* XK_ of the source, with XK_REV */
    WORD planes;                /* the destination: XD_16, or 1, 2, 4, 8 */
    BOOL transparent;           /* XK_1: leave the pixels of the 0 bits alone */
    const UBYTE *src;           /* the byte of the source with its first pixel (the rectangle's top left) */
    WORD bit;                   /* XK_1: the bit of it, 0 for the top one, to 7 */
    LONG pitch;                 /* bytes per source line */
    ULONG sw, sh;               /* the source rectangle's size */
    ULONG dw, dh;               /* the destination rectangle's size */
    ULONG skipx, skipy;         /* where the part drawn starts in it */
    LONG count, rows;           /* the size of the part drawn */
    UWORD *map;                 /* XK_8: the destination pixel of each index (256);
                                   with planes, 0xffff: not asked yet */
    UWORD fg, bg;               /* XK_1: the destination pixels of the 1 and the 0 bits */
    XferPenFn pen_rgb;          /* direct sources onto planes */
    XferPenFn pen_index;        /* XK_8 onto planes */
    void *ctx;                  /* handed to both */
} XferJob;

typedef struct
{
    const XferJob *job;
    XferAxis x0, y;
    BOOL filtered;              /* a reduction of a direct source onto 16 bits is filtered */
    LONG prev_y;                /* the source row of the last row made, -1: none */
    ULONG ckey[8];              /* the last colours asked of pen_rgb */
    UWORD cval[8];
} XferState;

void xfer_begin(XferState *s, const XferJob *job);

/*
 * The next row of a 16-bit destination at 'dst' (count pixels, written as
 * big-endian words).  'prev' is the row made before it, or NULL: an
 * enlargement repeats it.  A transparent XK_1 source writes the 1 bits only.
 */
void xfer_row16(XferState *s, UBYTE *dst, const UBYTE *prev);

/*
 * The next row for planes: count pixel values in 'pix' (a row the same
 * source row gave before stays), and, for a transparent source, in 'mask'
 * 1 for the pixels to write.
 */
void xfer_row_pix(XferState *s, UBYTE *pix, UBYTE *mask);

/*
 * Write count pixels from column x0 of a line of interleaved planes
 * ('line' is the first word of the line); the bits of other pixels stay.
 * With 'mask', the pixels it has 0 for stay too.
 */
void xfer_store_planar(UBYTE *line, WORD planes, LONG x0, LONG count,
                       const UBYTE *pix, const UBYTE *mask);

#endif /* VDI_XFERCORE_H */
