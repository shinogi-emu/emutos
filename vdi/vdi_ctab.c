/*
 * vdi_ctab.c - colour tables and inverse colour tables
 *
 * vs_ctab, vs_ctab_entry and vs_dflt_ctab (205), vq_ctab and the other
 * colour table calls (206), v_create_itab and v_delete_itab (208), as fVDI
 * has them (engine/ctab.c, itable.c and colour.c of the fVDI fork).
 *
 * A colour table here lists the colours of the workstation by pixel value:
 * entry i is the colour asked for (vs_color() and friends) for the VDI pen
 * whose pixel value is i (REV_MAP_COL[i]).  On a 16-bit screen the pixel
 * values are those of the workstation's pseudo-palette, so the table has
 * 256 entries there too, ordered the same way.
 *
 * The VDI keeps two snapshots per workstation, made when first needed: the
 * colours by pixel value (vq_ctab_id(), and the palette that dithering onto
 * planes aims for) and by VDI pen (the palette of an 8-bit bitmap that has
 * no table, see vdi_xfer.c).  Each is compared with the pens when it is
 * used; a change gives the first one a new id.  Each keeps the inverse
 * table made for it, at 4 bits a component, until the colours change.
 *
 * Tables a program makes with v_create_ctab are its to read and write:
 * each is a block of its own, in memory every program can reach, holding
 * the table only (the VDI's record of it is elsewhere, in its own memory).
 * Inverse tables are the VDI's, the program only gets a number for one,
 * never the same twice.  Both kinds belong to the workstation and go when
 * it is closed (see vdi_main.c), or with v_delete_ctab and v_delete_itab;
 * the program's pointer to a table is no good after either.
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "lineavars.h"
#include "string.h"
#include "bdosdefs.h"
#include "vdi_kept.h"
#include "vdi_nvdimath.h"
#include "vdi_xfercore.h"

#if CONF_WITH_VDI_NVDI_EXT

/* the memory of a table a program made: it reads and writes it (Mxalloc() global) */
#define CTAB_SHARED_MODE    (MX_PREFTTRAM | 0x20)

/* the bits a component of the inverse tables the VDI makes for its snapshots */
#define CTAB_ITAB_BITS      4

/*
 * The ids and numbers given so far (no initialised data: the ROM has none).
 * The ids 1-32 name the default tables by bits a pixel, as fVDI's; the
 * others start at 33.  Inverse tables are numbered from 1.
 */
#define FIRST_ID    33
static ULONG ids_given;
static ULONG refs_given;


/* a longword out of two words: they need not be aligned for one */
static LONG get_long(const WORD *p)
{
    return ((LONG)p[0] << 16) | (LONG)(UWORD)p[1];
}


static void put_long(WORD *p, LONG v)
{
    p[0] = (WORD)(v >> 16);
    p[1] = (WORD)v;
}


/* a new id for a colour table; 0 when they have run out */
static LONG new_id(void)
{
    if (ids_given > 0x7fffffffUL - FIRST_ID)
        return 0;
    return (LONG)(FIRST_ID + ids_given++);
}


/* the colours of the workstation */
static WORD palette_size(void)
{
    return (numcolors > 256) ? 256 : numcolors;
}


static void ctab_header(COLOR_TAB *t, WORD count, LONG id)
{
    memset(t, 0, COLOR_TAB_HEADER);
    t->magic = COLOR_TAB_MAGIC;
    t->length = COLOR_TAB_BYTES(count);
    t->map_id = id;
    t->color_space = 1;
    t->no_colors = count;
}


/* a colour of 0-1000 components as a COLOR_ENTRY */
static void entry_of(COLOR_ENTRY *e, const WORD *rgb)
{
    e->rgb.reserved = 0;
    e->rgb.red = vdi_to_rgb16(rgb[0]);
    e->rgb.green = vdi_to_rgb16(rgb[1]);
    e->rgb.blue = vdi_to_rgb16(rgb[2]);
}


/* the colour asked for a VDI pen as a COLOR_ENTRY */
static void pen_entry(Vwk *vwk, WORD pen, COLOR_ENTRY *e)
{
    WORD rgb[3];

    vdi_pen_request(vwk, pen, rgb);
    entry_of(e, rgb);
}


/*
 * The default table of 'bits' bits a pixel (1, 2, 4, 8, 15, 16, 24 or 32):
 * the colours the pens start with, by pixel value as TOS maps the pens (the
 * last value is pen 1; with more than 16 colours, value 15 is the last pen).
 * FALSE for other bits.
 */
static WORD default_count(LONG bits)
{
    switch (bits)
    {
    case 1:
    case 2:
    case 4:
    case 8:
        return (WORD)(1 << bits);
    case 15:
    case 16:
    case 24:
    case 32:
        return 256;
    default:
        return 0;
    }
}

static BOOL default_table(COLOR_TAB *t, LONG bits)
{
    static const SBYTE pens[16] = { 0, 2, 3, 6, 4, 7, 5, 8, 9, 10, 11, 14, 12, 15, 13, -1 };
    WORD count = default_count(bits), i, pen, rgb[3];

    if (count == 0)
        return FALSE;
    ctab_header(t, count, bits);
    for (i = 0; i < count; i++)
    {
        pen = i;
        if (i == count - 1)
            pen = 1;
        else if (i < 16)
            pen = (pens[i] < 0) ? count - 1 : pens[i];
        vdi_default_pen_rgb(pen, rgb);
        entry_of(&t->colors[i], rgb);
    }
    return TRUE;
}


/*
 * TRUE if a program's colour table can be read as one: aligned, the magic,
 * format 0, RGB, 1-256 colours, long enough for them
 */
BOOL vdi_ctab_valid(const COLOR_TAB *ctab)
{
    return ctab && !((ULONG)ctab & 1) &&
           ctab->magic == COLOR_TAB_MAGIC && ctab->format == 0 &&
           ctab->color_space == 1 && ctab->no_colors > 0 && ctab->no_colors <= 256 &&
           ctab->length >= COLOR_TAB_BYTES(ctab->no_colors);
}


/*
 * Copy the header of a program's colour table once, and say whether the
 * copy describes a table that can be read (as vdi_ctab_valid()).  The
 * program may change its table at any time: only the copy is looked at,
 * and the colours are read as far as the copy's count says.  A NULL or odd
 * pointer is not read (the copy then has no colours).
 */
BOOL vdi_ctab_head(const COLOR_TAB *ctab, COLOR_TAB_HEAD *head)
{
    if (!ctab || ((ULONG)ctab & 1))
    {
        memset(head, 0, sizeof(*head));
        return FALSE;
    }
    memcpy(head, ctab, sizeof(*head));
    return head->magic == COLOR_TAB_MAGIC && head->format == 0 &&
           head->color_space == 1 && head->no_colors > 0 && head->no_colors <= 256 &&
           head->length >= COLOR_TAB_BYTES(head->no_colors);
}


/*
 * The lists of the workstation
 */

void vdi_ctab_init(Vwk *vwk)
{
    vwk->ctabs = NULL;
    vwk->itabs = NULL;
}


/* unlink a table from the list of the workstation, if it is there */
static void unlink_ctab(Vwk *vwk, VdiCtab *node)
{
    VdiCtab **link;

    for (link = &vwk->ctabs; *link; link = &(*link)->next)
    {
        if (*link == node)
        {
            *link = node->next;
            return;
        }
    }
}


/* free an unlinked table, its inverse table and, for one a program made, its block (may sleep) */
static void free_ctab(VdiCtab *node)
{
    if (node->inverse)
        vdi_free_kept(node->inverse);
    if (node->kind == VDI_CTAB_CREATED && node->table)
        vdi_free_kept(node->table);
    vdi_free_kept(node);
}


/*
 * Free the tables of a workstation that is closed.  Each is unlinked before
 * it is freed (the free may sleep), so a release that has to be done again
 * (vdi_main.c) finds only what is left.
 */
void vdi_ctab_release(Vwk *vwk)
{
    VdiCtab *c;
    VdiItab *t;

    while ((c = vwk->ctabs) != NULL)
    {
        vwk->ctabs = c->next;
        free_ctab(c);
    }
    while ((t = vwk->itabs) != NULL)
    {
        vwk->itabs = t->next;
        vdi_free_kept(t);
    }
}


/*
 * a table node of 'count' colours, not linked (may sleep): a snapshot's table
 * follows the node, in the VDI's memory; one a program made is a block of
 * its own that every program can write (the program writes nothing of the
 * VDI's then, whatever it writes in its block)
 */
static VdiCtab *alloc_ctab(WORD count, WORD kind)
{
    VdiCtab *node;
    COLOR_TAB *table;

    if (kind == VDI_CTAB_CREATED)
    {
        node = (VdiCtab *)vdi_alloc_kept(sizeof(VdiCtab));
        table = (COLOR_TAB *)vdi_alloc_kept_mode(COLOR_TAB_BYTES(count), CTAB_SHARED_MODE);
        if (!node || !table)
        {
            if (node)
                vdi_free_kept(node);
            if (table)
                vdi_free_kept(table);
            return NULL;
        }
    }
    else
    {
        node = (VdiCtab *)vdi_alloc_kept((LONG)sizeof(VdiCtab) + COLOR_TAB_BYTES(count));
        if (!node)
            return NULL;
        table = (COLOR_TAB *)(node + 1);
    }
    node->next = NULL;
    node->table = table;
    node->inverse = NULL;
    node->kind = kind;
    node->pad = 0;
    ctab_header(table, count, 0);
    return node;
}


static VdiCtab *find_kind(Vwk *vwk, WORD kind)
{
    VdiCtab *node;

    for (node = vwk->ctabs; node; node = node->next)
        if (node->kind == kind)
            return node;
    return NULL;
}


/* bring a snapshot up to the pens; a changed table by pixel value gets a new id */
static void refresh(Vwk *vwk, VdiCtab *node)
{
    COLOR_TAB *t = node->table;
    COLOR_ENTRY e;
    WORD i, count = (WORD)t->no_colors;
    BOOL changed = FALSE;

    for (i = 0; i < count; i++)
    {
        pen_entry(vwk, (node->kind == VDI_CTAB_CURRENT) ? REV_MAP_COL[i] : i, &e);
        if (memcmp(&e, &t->colors[i], sizeof(e)))
        {
            t->colors[i] = e;
            changed = TRUE;
        }
    }
    if (changed || t->map_id == 0)
        t->map_id = new_id();
}


/*
 * The snapshot of a kind (VDI_CTAB_CURRENT or _PENS), brought up to the
 * pens; with 'make' it is made if there is none (that may sleep), else NULL.
 */
static VdiCtab *snapshot(Vwk *vwk, WORD kind, BOOL make)
{
    WORD count = palette_size();
    VdiCtab *node, *fresh;

    node = find_kind(vwk, kind);
    if (node && node->table->no_colors == count)
    {
        refresh(vwk, node);
        return node;
    }
    if (!make)
        return NULL;
    fresh = alloc_ctab(count, kind);
    if (!fresh)
        return NULL;
    /* whatever happened meanwhile, the old ones of the kind go */
    while ((node = find_kind(vwk, kind)) != NULL)
    {
        unlink_ctab(vwk, node);
        free_ctab(node);
    }
    fresh->next = vwk->ctabs;
    vwk->ctabs = fresh;
    refresh(vwk, fresh);
    return fresh;
}


/* TRUE if an inverse table was made for these colours */
static BOOL itab_fits(const VdiItab *t, const COLOR_TAB *ctab)
{
    return t && t->count == ctab->no_colors &&
           !memcmp(t->colors, ctab->colors, 8L * t->count);
}


/*
 * an inverse table of 'count' colours at 'bits' bits a component, with its
 * snapshot and map not filled in (may sleep)
 */
static VdiItab *alloc_itab(WORD count, WORD bits)
{
    VdiItab *t = (VdiItab *)vdi_alloc_kept((LONG)sizeof(VdiItab) + 8L * count + XFER_ITAB_CELLS(bits));

    if (t)
    {
        t->next = NULL;
        t->ref = 0;
        t->count = count;
        t->bits = bits;
    }
    return t;
}


/* the snapshot and the map of an inverse table, from colours that stay while it runs */
static void fill_itab(VdiItab *t, const COLOR_ENTRY *colors, void *work)
{
    memcpy(t->colors, colors, 8L * t->count);
    xfer_itab_fill(t->colors, t->count, t->bits, (UBYTE *)VDI_ITAB_MAP(t), work);
}


/*
 * Make sure the snapshot of a kind is there and, with 'inverse', that it has
 * an inverse table for its colours.  This may sleep: what it finds is looked
 * for again afterwards, and vdi_ctab_get() gives it without sleeping.
 */
BOOL vdi_ctab_prepare(Vwk *vwk, WORD kind, BOOL inverse)
{
    VdiCtab *node = snapshot(vwk, kind, TRUE);
    VdiItab *t, *old;
    void *work;
    WORD count;

    if (!node)
        return FALSE;
    if (!inverse || itab_fits(node->inverse, node->table))
        return TRUE;
    count = (WORD)node->table->no_colors;
    t = alloc_itab(count, CTAB_ITAB_BITS);
    work = vdi_alloc_kept(XFER_ITAB_WORK(count));
    node = snapshot(vwk, kind, FALSE);
    if (node && t && work && node->table->no_colors == count)
    {
        fill_itab(t, node->table->colors, work);
        old = node->inverse;
        node->inverse = t;
        t = old;            /* freed below */
    }
    else
        node = NULL;
    if (t)
        vdi_free_kept(t);
    if (work)
        vdi_free_kept(work);
    return node != NULL;
}


/*
 * The snapshot of a kind, brought up to the pens, and with 'inverse' only if
 * its inverse table still fits; NULL if not (vdi_ctab_prepare() first).
 * It does not sleep.
 */
const VdiCtab *vdi_ctab_get(Vwk *vwk, WORD kind, BOOL inverse)
{
    VdiCtab *node = snapshot(vwk, kind, FALSE);

    if (!node || (inverse && !itab_fits(node->inverse, node->table)))
        return NULL;
    return node;
}


/*
 * The inverse table 'ref' of the workstation, if it was made for the colours
 * of 'ctab' (they are compared: a table changed since is refused).  The
 * program's table is read only if 'ref' is one of the workstation's: its
 * header once, then as many colours as the inverse table has.
 */
const VdiItab *vdi_itab_find(Vwk *vwk, ULONG ref, const COLOR_TAB *ctab)
{
    const VdiItab *t;
    COLOR_TAB_HEAD head;

    for (t = vwk->itabs; t; t = t->next)
    {
        if (t->ref == ref)
            return (vdi_ctab_head(ctab, &head) && head.no_colors == t->count &&
                    !memcmp(t->colors, ctab->colors, 8L * t->count)) ? t : NULL;
    }
    return NULL;
}


/*
 * The palette calls
 */

/* set 'count' pens from 'first' (pixel values) to the colours of a table */
static WORD set_entries(Vwk *vwk, WORD first, LONG count, const COLOR_ENTRY *colors)
{
    WORD size = palette_size(), i, rgb[3];

    if (first < 0 || first >= size || count <= 0)
        return 0;
    if (count > size - first)
        count = size - first;
    for (i = 0; i < count; i++)
    {
        rgb[0] = rgb16_to_vdi(colors[i].rgb.red);
        rgb[1] = rgb16_to_vdi(colors[i].rgb.green);
        rgb[2] = rgb16_to_vdi(colors[i].rgb.blue);
        vdi_set_pen_rgb(vwk, REV_MAP_COL[first + i], rgb);
    }
    return (WORD)count;
}


/*
 * 205: contrl[5]
 * 0 vs_ctab: intin the colour table itself (CONTRL[3] words of it)
 * 1 vs_ctab_entry: intin[0] the pixel value, [1-2] colour space, [3-6] the COLOR_ENTRY
 * 2 vs_dflt_ctab
 * intout[0]: the number of entries set, 0 if refused
 */
void vdi_vs_ctab(Vwk *vwk)
{
    const COLOR_TAB *t = (const COLOR_TAB *)INTIN;
    LONG space;
    WORD done = 0, i, rgb[3];

    switch (CONTRL[5])
    {
    case 0:
        if (CONTRL[3] >= COLOR_TAB_HEADER / 2 && vdi_ctab_valid(t) &&
            (LONG)CONTRL[3] * 2 >= COLOR_TAB_BYTES(t->no_colors))
            done = set_entries(vwk, 0, t->no_colors, t->colors);
        break;
    case 1:
        if (CONTRL[3] >= 7)
        {
            space = get_long(INTIN + 1);
            if (space == 0 || space == 1)
                done = set_entries(vwk, INTIN[0], 1, (const COLOR_ENTRY *)(INTIN + 3));
        }
        break;
    case 2:
        done = palette_size();
        for (i = 0; i < done; i++)
        {
            vdi_default_pen_rgb(i, rgb);
            vdi_set_pen_rgb(vwk, i, rgb);
        }
        break;
    }
    INTOUT[0] = done;
}


/* the pixel value of a VDI pen, -1 if there is none */
static WORD pen_value(WORD pen)
{
    return (pen < 0 || pen >= palette_size()) ? -1 : MAP_COL[pen];
}


/*
 * 206: contrl[5]
 * 0 vq_ctab: intin[0-1] the bytes the caller has room for; intout the table
 * 1 vq_ctab_entry: intin[0] a pixel value; intout[0-1] colour space (0: no
 *   such value), [2-5] the COLOR_ENTRY
 * 2 vq_ctab_id: intout[0-1] the id of the table
 * 3 v_ctab_idx2vdi: intin[0] a pixel value; intout[0] its VDI pen, -1 if none
 * 4 v_ctab_vdi2idx: intin[0] a VDI pen; intout[0] its pixel value, -1 if none
 * 5 v_ctab_idx2value: intin[0] a pixel value; intout[0-1] the pixel as
 *   written to the screen (that of pen 1 for a value that is not one)
 * 6 v_get_ctab_id: intout[0-1] a new id
 * 7 vq_dflt_ctab: intin[0-1] as vq_ctab; intout the default table
 * 8 v_create_ctab: intin[0-1] colour space, [2-3] pixel format; intout[0-1]
 *   the table, NULL if refused
 * 9 v_delete_ctab: intin[0-1] the table; intout[0] 1 if deleted
 * CONTRL[4] is set to what was written: the tables give length/2 words, 0 if refused.
 */
void vdi_vq_ctab(Vwk *vwk)
{
    const VdiCtab *node;
    COLOR_ENTRY e;
    VdiCtab *c;
    LONG length, value, bits, id;
    WORD index, n = 0;

    switch (CONTRL[5])
    {
    case 0:
    case 7:
        if (CONTRL[3] < 2)
            break;
        if (CONTRL[5] == 0)
        {
            length = COLOR_TAB_BYTES(palette_size());
            if (get_long(INTIN) < length)
                break;
            node = snapshot(vwk, VDI_CTAB_CURRENT, TRUE);  /* may sleep */
            if (!node)
                break;
            memcpy(INTOUT, node->table, length);
        }
        else
        {
            bits = (v_planes > 8) ? 16 : v_planes;
            length = COLOR_TAB_BYTES(default_count(bits));
            if (get_long(INTIN) < length || !default_table((COLOR_TAB *)INTOUT, bits))
                break;
        }
        n = (WORD)(length / 2);
        break;
    case 1:
        index = (CONTRL[3] >= 1) ? INTIN[0] : -1;
        value = 0;
        e.rgb.reserved = e.rgb.red = e.rgb.green = e.rgb.blue = 0;
        if (index >= 0 && index < palette_size())
        {
            value = 1;
            pen_entry(vwk, REV_MAP_COL[index], &e);
        }
        put_long(INTOUT, value);
        INTOUT[2] = e.rgb.reserved;
        INTOUT[3] = (WORD)e.rgb.red;
        INTOUT[4] = (WORD)e.rgb.green;
        INTOUT[5] = (WORD)e.rgb.blue;
        n = 6;
        break;
    case 2:
        node = snapshot(vwk, VDI_CTAB_CURRENT, TRUE);  /* may sleep */
        put_long(INTOUT, node ? node->table->map_id : 0);
        n = 2;
        break;
    case 3:
        index = (CONTRL[3] >= 1) ? INTIN[0] : -1;
        INTOUT[0] = (index >= 0 && index < palette_size()) ? REV_MAP_COL[index] : -1;
        n = 1;
        break;
    case 4:
        INTOUT[0] = pen_value((CONTRL[3] >= 1) ? INTIN[0] : -1);
        n = 1;
        break;
    case 5:
        index = (CONTRL[3] >= 1) ? INTIN[0] : -1;
        if (index < 0 || index >= palette_size())
            index = MAP_COL[1];
#if CONF_WITH_VDI_16BIT
        if (TRUECOLOR_MODE)
            value = vwk->ext->palette[index];
        else
#endif
            value = index;
        put_long(INTOUT, value);
        n = 2;
        break;
    case 6:
        put_long(INTOUT, new_id());
        n = 2;
        break;
    case 8:
        c = NULL;
        if (CONTRL[3] >= 4)
        {
            value = get_long(INTIN);            /* the colour space: 0 (the current one) or 1 (RGB) */
            bits = get_long(INTIN + 2) & 255;   /* the bits of the pixel format */
            if ((value == 0 || value == 1) && default_count(bits))
            {
                c = alloc_ctab(default_count(bits), VDI_CTAB_CREATED);     /* may sleep */
                if (c)
                {
                    default_table(c->table, bits);
                    id = new_id();
                    c->table->map_id = id;
                    c->next = vwk->ctabs;
                    vwk->ctabs = c;
                }
            }
        }
        put_long(INTOUT, c ? (LONG)c->table : 0L);
        n = 2;
        break;
    case 9:
        INTOUT[0] = 0;
        if (CONTRL[3] >= 2)
        {
            value = get_long(INTIN);
            for (c = vwk->ctabs; c; c = c->next)
            {
                /* the pointer is compared, never read through */
                if (c->kind == VDI_CTAB_CREATED && (LONG)c->table == value)
                {
                    unlink_ctab(vwk, c);
                    free_ctab(c);                   /* may sleep */
                    INTOUT[0] = 1;
                    break;
                }
            }
        }
        n = 1;
        break;
    }
    CONTRL[4] = n;
}


/*
 * 208: contrl[5]
 * 0 v_create_itab: intin[0-1] a colour table, [2] bits a component (3-5);
 *   intout[0-1] the number of the inverse table, 0 if refused
 * 1 v_delete_itab: intin[0-1] that number; intout[0] 1 if deleted
 */
void vdi_v_itab(Vwk *vwk)
{
    const COLOR_TAB *ctab;
    VdiItab *t, **link;
    void *work;
    LONG ref = 0;
    WORD bits, count;

    switch (CONTRL[5])
    {
    case 0:
        if (CONTRL[3] >= 3)
        {
            ctab = (const COLOR_TAB *)get_long(INTIN);
            bits = INTIN[2];
            if (vdi_ctab_valid(ctab) && bits >= XFER_ITAB_MIN_BITS && bits <= XFER_ITAB_MAX_BITS &&
                refs_given < 0xffffffffUL)
            {
                count = (WORD)ctab->no_colors;
                t = alloc_itab(count, bits);                    /* may sleep */
                work = vdi_alloc_kept(XFER_ITAB_WORK(count));   /* may sleep */
                /* the table is the program's: it is read again after the sleeps */
                if (t && work && vdi_ctab_valid(ctab) && ctab->no_colors == count &&
                    refs_given < 0xffffffffUL)
                {
                    fill_itab(t, ctab->colors, work);
                    t->ref = ++refs_given;
                    ref = (LONG)t->ref;
                    t->next = vwk->itabs;
                    vwk->itabs = t;
                    t = NULL;
                }
                if (t)
                    vdi_free_kept(t);
                if (work)
                    vdi_free_kept(work);
            }
        }
        put_long(INTOUT, ref);
        CONTRL[4] = 2;
        break;
    case 1:
        INTOUT[0] = 0;
        if (CONTRL[3] >= 2)
        {
            ref = get_long(INTIN);
            for (link = &vwk->itabs; *link; link = &(*link)->next)
            {
                if ((LONG)(*link)->ref == ref)
                {
                    t = *link;
                    *link = t->next;
                    vdi_free_kept(t);                   /* may sleep */
                    INTOUT[0] = 1;
                    break;
                }
            }
        }
        CONTRL[4] = 1;
        break;
    default:
        CONTRL[4] = 0;
        break;
    }
}

#endif /* CONF_WITH_VDI_NVDI_EXT */
