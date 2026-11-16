/*
 * vdi_nvdiext.h - the data structures of the NVDI colour, raster and
 * off-screen bitmap calls
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_NVDIEXT_H
#define VDI_NVDIEXT_H

#include "portab.h"

/* a rectangle with 16-bit corners (vr_clip_rects_by_dst() and friends) */
typedef struct
{
    WORD x1;
    WORD y1;
    WORD x2;
    WORD y2;
} RECT16;

/* one colour, each component 0-65535 */
typedef struct
{
    UWORD reserved;
    UWORD red;
    UWORD green;
    UWORD blue;
} COLOR_RGB;

typedef struct
{
    UWORD cyan;
    UWORD magenta;
    UWORD yellow;
    UWORD black;
} COLOR_CMYK;

typedef union
{
    COLOR_RGB rgb;
    COLOR_CMYK cmyk;
} COLOR_ENTRY;

/* a colour table, as a program hands it to the VDI (read only here) */
#define COLOR_TAB_MAGIC 0x63746162L     /* 'ctab' */

typedef struct
{
    LONG magic;                 /* COLOR_TAB_MAGIC */
    LONG length;                /* the size of the structure in bytes */
    LONG format;
    LONG reserved;
    LONG map_id;
    LONG color_space;
    LONG flags;
    LONG no_colors;             /* the number of entries of colors[] in use */
    LONG reserved1;
    LONG reserved2;
    LONG reserved3;
    LONG reserved4;
    COLOR_ENTRY colors[256];
} COLOR_TAB;

/* the bytes of a colour table of 'n' colours: the header, then the colours */
#define COLOR_TAB_HEADER    48
#define COLOR_TAB_BYTES(n)  (COLOR_TAB_HEADER + 8L * (n))

/* the header of a colour table alone: a copy of a program's, read once */
typedef struct
{
    LONG magic;
    LONG length;
    LONG format;
    LONG reserved;
    LONG map_id;
    LONG color_space;
    LONG flags;
    LONG no_colors;
    LONG reserved1;
    LONG reserved2;
    LONG reserved3;
    LONG reserved4;
} COLOR_TAB_HEAD;

/*
 * An inverse colour table (v_create_itab, and those the VDI keeps for its
 * own tables): a snapshot of the palette it was made for, then its map,
 * 1 << (3 * bits) bytes (see xfer_itab_fill() in vdi_xfercore.h).  The
 * caller only gets 'ref', which the VDI never gives twice.
 */
typedef struct VdiItab_ VdiItab;
struct VdiItab_
{
    VdiItab *next;
    ULONG ref;
    WORD count;                 /* the colours of the snapshot, 1-256 */
    WORD bits;                  /* 3-5 bits a component */
    COLOR_ENTRY colors[];
};

#define VDI_ITAB_MAP(t)     ((const UBYTE *)((t)->colors + (t)->count))

/*
 * A colour table of a workstation (see vdi_ctab.c): one a program made with
 * v_create_ctab (it has the pointer to 'table'), or a snapshot the VDI keeps
 * of the workstation's colours, with the inverse table made for it.
 */
typedef struct VdiCtab_ VdiCtab;
struct VdiCtab_
{
    VdiCtab *next;
    WORD kind;                  /* VDI_CTAB_CREATED, _CURRENT or _PENS */
    VdiItab *inverse;           /* the snapshots: the inverse table of 'table', or NULL */
    COLOR_TAB table;            /* only as many colours as no_colors are allocated */
};

#define VDI_CTAB_CREATED    0   /* made by v_create_ctab */
#define VDI_CTAB_CURRENT    1   /* the colours by pixel value (vq_ctab) */
#define VDI_CTAB_PENS       2   /* the colours by VDI pen: an 8-bit bitmap without a table */

/* a bitmap description, as v_open_bm() and v_resize_bm() take it */
#define GCBITMAP_MAGIC  0x6362746dL     /* 'cbtm' */

typedef struct
{
    LONG magic;                 /* GCBITMAP_MAGIC */
    LONG length;                /* the size of the structure in bytes */
    LONG format;
    LONG reserved;
    UBYTE *addr;                /* the pixels */
    LONG width;                 /* bytes per line */
    LONG bits;                  /* bits per pixel */
    ULONG px_format;
    LONG xmin, ymin, xmax, ymax;
    COLOR_TAB *ctab;
    void *itab;
    LONG reserved0;
    LONG reserved1;
} GCBITMAP;

#endif /* VDI_NVDIEXT_H */
