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
