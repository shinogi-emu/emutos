/*
 * vdi_nvdicol.c - the NVDI colour calls
 *
 * v_setrgb (138), vs_/vq_ fg_color and bg_color (200-203), v_color2value
 * and its relatives (204), and the highlight, minimum, maximum and weight
 * colours (207, 209).
 *
 * An RGB colour for text, fill, line or marker is drawn exactly on a
 * 16-bit screen: it goes into one of the extra pseudo-palette entries of
 * the workstation (pens 256 and up, see VDI_PAL_RGB()), and the colour
 * field of the attribute (text_color, ...) holds that pen.  On a screen
 * with a palette it becomes the nearest VDI pen.  The raster colours
 * (kind 4) are only kept.  Queries answer the colour as it was asked for;
 * for a pen, the colour that was requested for it.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "vdi_nvdimath.h"
#include "lineavars.h"

/* what a workstation starts with: black highlight and minimum, white maximum, even weight */
static const COLOR_RGB extra_default[4] =
{
    { 0, 0x0000, 0x0000, 0x0000 },
    { 0, 0x0000, 0x0000, 0x0000 },
    { 0, 0xffff, 0xffff, 0xffff },
    { 0, 0x8000, 0x8000, 0x8000 }
};


/*
 * the state of a workstation that starts from nothing: colours by pen,
 * raster colours black (pen 1) on white (pen 0)
 */
void vdi_color_init(Vwk *vwk)
{
    VwkColors *col = &vwk->col;
    WORD kind, i;

    for (kind = 0; kind < VDI_COL_KINDS; kind++)
    {
        col->req[kind][0].reserved = col->req[kind][1].reserved = 0;
        col->req[kind][0].red = col->req[kind][0].green = col->req[kind][0].blue = 0;
        col->req[kind][1].red = col->req[kind][1].green = col->req[kind][1].blue = 0;
        col->pen[kind][0] = 1;
        col->pen[kind][1] = 0;
    }
    col->rgbmask = 0;
    for (i = 0; i < 4; i++)
        col->extra[i] = extra_default[i];
#if CONF_WITH_VDI_16BIT
    if (vwk->ext)
    {
        for (i = 256; i < VWKEXT_PALETTE; i++)
            vwk->ext->palette[i] = 0;
    }
#endif
}


/* the colour field of the foreground of a kind of the workstation */
static WORD *kind_field(Vwk *vwk, WORD kind)
{
    switch (kind)
    {
    case VDI_COL_TEXT:
        return &vwk->text_color;
    case VDI_COL_FILL:
        return &vwk->fill_color;
    case VDI_COL_LINE:
        return &vwk->line_color;
    default:
        return &vwk->mark_color;
    }
}


/*
 * the requested colour of a VDI pen, as a COLOR_RGB
 */
void vdi_pen_rgb(Vwk *vwk, WORD pen, COLOR_RGB *c)
{
    WORD rgb[3];

    if (pen < 0 || pen >= numcolors)
        pen = 0;
    vdi_pen_request(vwk, pen, rgb);
    c->reserved = 0;
    c->red = vdi_to_rgb16(rgb[0]);
    c->green = vdi_to_rgb16(rgb[1]);
    c->blue = vdi_to_rgb16(rgb[2]);
}


/*
 * the VDI pen nearest a colour, among the pens of the screen
 */
WORD vdi_nearest_pen(const COLOR_RGB *c)
{
    return vdi_nearest_pen_in(get_vwk_by_handle(1)  /* the physical workstation */, c);
}


/*
 * the pixel format of the screen as vq_px_format() reports it
 */
ULONG vdi_px_format(void)
{
    if (v_planes == 1)
        return 0x01020101UL;
    if (v_planes == 2 || v_planes == 4 || v_planes == 8)
        return 0x01000000UL | ((ULONG)v_planes << 8) | (ULONG)v_planes;
    if (v_planes == 16)
        return 0x03021010UL;
    return 0;
}


/*
 * the pixel value of a colour: 16-bit screens, the packed pixel; the
 * others, the hardware pen of the nearest pen
 */
ULONG vdi_rgb_to_value(const COLOR_RGB *c)
{
#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE)
        return vdi_pack16(c->red, c->green, c->blue);
#endif
    return (ULONG)MAP_COL[vdi_nearest_pen(c)];
}


/* the colour of a pixel value; FALSE if the screen has no such value */
static BOOL value_to_rgb(Vwk *vwk, ULONG value, COLOR_RGB *c)
{
    c->reserved = 0;
#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE)
    {
        vdi_unpack16((UWORD)value, &c->red, &c->green, &c->blue);
        return TRUE;
    }
#endif
    if (value < (ULONG)numcolors)
    {
        vdi_pen_rgb(vwk, REV_MAP_COL[value], c);
        return TRUE;
    }
    c->red = c->green = c->blue = 0;
    return FALSE;
}


/*
 * the colour a screen with a palette shows for a colour: the nearest
 * pen's own, not the one asked for
 */
static void nearest_rgb(Vwk *vwk, const COLOR_RGB *c, COLOR_RGB *out)
{
    value_to_rgb(vwk, vdi_rgb_to_value(c), out);
}


/* a longword out of two words of intin: they need not be aligned for one */
static LONG get_long(const WORD *p)
{
    return ((LONG)p[0] << 16) | (LONG)(UWORD)p[1];
}


static void put_long(WORD *p, LONG v)
{
    p[0] = (WORD)(v >> 16);
    p[1] = (WORD)v;
}


/* a COLOR_ENTRY of intin (reserved, red, green, blue) */
static void get_entry(const WORD *p, COLOR_RGB *c)
{
    c->reserved = 0;
    c->red = (UWORD)p[1];
    c->green = (UWORD)p[2];
    c->blue = (UWORD)p[3];
}


static void put_entry(WORD *p, const COLOR_RGB *c)
{
    p[0] = 0;
    p[1] = (WORD)c->red;
    p[2] = (WORD)c->green;
    p[3] = (WORD)c->blue;
}


/*
 * set the foreground or the background of a kind of colour to an RGB colour
 */
static void set_kind(Vwk *vwk, WORD kind, WORD bg, const COLOR_RGB *c)
{
    VwkColors *col = &vwk->col;
    UWORD bit = (UWORD)1 << (2 * kind + bg);

    col->req[kind][bg] = *c;
    col->pen[kind][bg] = vdi_nearest_pen_in(vwk, c);

#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE)
    {
        col->rgbmask |= bit;
        if (kind < VDI_COL_RASTER)
        {
            vwk->ext->palette[VDI_PAL_RGB(kind, bg)] = vdi_pack16(c->red, c->green, c->blue);
            if (!bg)
                *kind_field(vwk, kind) = VDI_PAL_RGB(kind, 0);
        }
        return;
    }
#endif
    /* a screen with a palette: the nearest pen is the colour */
    col->rgbmask &= ~bit;
    if (!bg && kind < VDI_COL_RASTER)
        *kind_field(vwk, kind) = MAP_COL[col->pen[kind][0]];
}


/*
 * the foreground or the background of a kind of colour, as the VDI
 * pen it comes from or the RGB colour it was asked for
 */
static void get_kind(Vwk *vwk, WORD kind, WORD bg, COLOR_RGB *c)
{
    VwkColors *col = &vwk->col;
    WORD pen;

    if (col->rgbmask & ((UWORD)1 << (2 * kind + bg)))
    {
        *c = col->req[kind][bg];
        return;
    }
    if (!bg && kind < VDI_COL_RASTER)
        pen = REV_MAP_COL[*kind_field(vwk, kind)];
    else
        pen = col->pen[kind][bg];
    vdi_pen_rgb(vwk, pen, c);
}


/* the colour space of the call, 0 (the current one) or 1 (RGB) are known */
static BOOL known_space(void)
{
    LONG space = get_long(INTIN);

    return space == 0 || space == 1;
}


/*
 * vs_fg_color (200) and vs_bg_color (201)
 *
 * intin[0-1] colour space, intin[2-5] the COLOR_ENTRY; contrl[5] the kind:
 * 0 text, 1 fill, 2 line, 3 marker, 4 raster.  intout[0] is 1 when done,
 * 0 for another colour space, -1 for another kind.
 */
static void set_call(Vwk *vwk, WORD bg)
{
    COLOR_RGB c;
    WORD kind = CONTRL[5];

    if (!known_space())
        INTOUT[0] = 0;
    else if (kind < 0 || kind >= VDI_COL_KINDS)
        INTOUT[0] = -1;
    else
    {
        get_entry(INTIN + 2, &c);
        set_kind(vwk, kind, bg, &c);
        INTOUT[0] = 1;
    }
}


void vdi_vs_fg_color(Vwk *vwk)
{
    set_call(vwk, 0);
}


void vdi_vs_bg_color(Vwk *vwk)
{
    set_call(vwk, 1);
}


/*
 * vq_fg_color (202) and vq_bg_color (203)
 *
 * intout[0-1] the colour space (1), intout[2-5] the colour; -1 in
 * intout[0-1] and nothing else for another kind.
 */
static void query_call(Vwk *vwk, WORD bg)
{
    COLOR_RGB c;
    WORD kind = CONTRL[5];

    if (kind < 0 || kind >= VDI_COL_KINDS)
    {
        put_long(INTOUT, -1L);
        CONTRL[4] = 2;
        return;
    }
    get_kind(vwk, kind, bg, &c);
    put_long(INTOUT, 1L);
    put_entry(INTOUT + 2, &c);
}


void vdi_vq_fg_color(Vwk *vwk)
{
    query_call(vwk, 0);
}


void vdi_vq_bg_color(Vwk *vwk)
{
    query_call(vwk, 1);
}


/*
 * v_setrgb (138): contrl[5] the kind, intin[0-2] red, green and blue
 * in 0-1000; the same as vs_fg_color of that colour
 */
void vdi_v_setrgb(Vwk *vwk)
{
    COLOR_RGB c;
    WORD kind = CONTRL[5];

    if (kind < 0 || kind >= VDI_COL_KINDS)
        return;
    c.reserved = 0;
    c.red = vdi_to_rgb16(INTIN[0]);
    c.green = vdi_to_rgb16(INTIN[1]);
    c.blue = vdi_to_rgb16(INTIN[2]);
    set_kind(vwk, kind, 0, &c);
}


/*
 * vs_hilite_color and company (207): contrl[5] 0 highlight, 1 minimum,
 * 2 maximum, 3 weight; the colour is only kept
 */
void vdi_vs_x_color(Vwk *vwk)
{
    WORD kind = CONTRL[5];

    if (!known_space())
        INTOUT[0] = 0;
    else if (kind < 0 || kind > 3)
        INTOUT[0] = -1;
    else
    {
        get_entry(INTIN + 2, &vwk->col.extra[kind]);
        INTOUT[0] = 1;
    }
}


/* the queries of them (209) */
void vdi_vq_x_color(Vwk *vwk)
{
    WORD kind = CONTRL[5];

    if (kind < 0 || kind > 3)
    {
        put_long(INTOUT, -1L);
        CONTRL[4] = 2;
        return;
    }
    put_long(INTOUT, 1L);
    put_entry(INTOUT + 2, &vwk->col.extra[kind]);
}


/*
 * 204: contrl[5]
 * 0 v_color2value: intin colour space and COLOR_ENTRY, intout[0-1] the pixel value
 * 1 v_value2color: intin[0-1] a pixel value, intout[0-1] colour space (0 if
 *   there is no such value), intout[2-5] the colour
 * 2 v_color2nearest: as 1, of the colour the screen shows nearest the one given
 * 3 vq_px_format: intout[0-1] colour space 1, intout[2-3] the pixel format
 */
void vdi_color_entry(Vwk *vwk)
{
    COLOR_RGB c, n;
    LONG space = 1;

    switch (CONTRL[5])
    {
    case 0:
        c.reserved = 0;
        if (known_space())
        {
            get_entry(INTIN + 2, &c);
            put_long(INTOUT, (LONG)vdi_rgb_to_value(&c));
        }
        else
            put_long(INTOUT, 0L);
        CONTRL[4] = 2;
        break;
    case 1:
        if (!value_to_rgb(vwk, (ULONG)get_long(INTIN), &c))
            space = 0;
        put_long(INTOUT, space);
        put_entry(INTOUT + 2, &c);
        CONTRL[4] = 6;
        break;
    case 2:
        if (known_space())
        {
            get_entry(INTIN + 2, &c);
            nearest_rgb(vwk, &c, &n);
        }
        else
        {
            n.reserved = 0;
            n.red = n.green = n.blue = 0;
            space = 0;
        }
        put_long(INTOUT, space);
        put_entry(INTOUT + 2, &n);
        CONTRL[4] = 6;
        break;
    case 3:
        put_long(INTOUT, vdi_px_format() ? 1L : 0L);
        put_long(INTOUT + 2, (LONG)vdi_px_format());
        CONTRL[4] = 4;
        break;
    default:
        CONTRL[4] = 0;
        break;
    }
}


#if CONF_WITH_VDI_16BIT
/* the text and the line background in effect during a drawing call (see vdi_defs.h) */
UWORD vdi_bg_text, vdi_bg_line;


/* the 16-bit palette index of the background of a kind of colour: pen 0, or the RGB colour set */
UWORD vdi_bg_index(const Vwk *vwk, WORD kind)
{
    if (TRUECOLOR_MODE && (vwk->col.rgbmask & ((UWORD)1 << (2 * kind + 1))))
        return VDI_PAL_RGB(kind, 1);
    return 0;
}
#endif
