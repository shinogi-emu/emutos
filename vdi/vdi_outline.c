/*
 * vdi_outline.c - outline fonts on the workstations
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The outline font engine (vdi_ft2.c) knows nothing of workstations.
 * This puts its fonts on them:
 *
 * - The first vst_load_fonts() made by any workstation registers the font
 *   files of C:\GEMSYS\FONTS with the engine.  Each face gets a font
 *   header, flagged F_OUTLINE_FONT and without a bitmap, which vqt_name()
 *   and vst_font() find after the fonts of font_ring[].  They are kept out
 *   of font_ring[] itself, which line-A programs walk, and which cannot
 *   grow.  A workstation sees them once it has called vst_load_fonts().
 *
 * - Selecting a face and a size gives the workstation a sized font from
 *   the engine, and copies its metrics into vwk->outline_head, which
 *   becomes vwk->cur_font: what only reads the font header, such as
 *   vqt_attributes() and vqt_fontinfo(), needs nothing more.  A size is
 *   made once, when it is selected, and lasts as long as the engine.
 *
 * - v_gtext() has the engine render the string into a one-plane bitmap,
 *   turns it here for rotated text, and draws it with text_blt() as a
 *   single character.  The text effects, the underline included, are the
 *   engine's.  Outline text is always clipped to the screen.
 *
 * - The engine is called on a stack of its own (outline_on_stack()).
 *
 * This file is compiled with -mshort like the rest of the VDI, so the
 * engine is only called through vdi_ft2.h, and sizes are LONGs.
 */

#include "emutos.h"
#include "string.h"
#include "vdi_defs.h"
#include "vdistub.h"
#include "lineavars.h"
#include "vdi_ft2.h"
#include "vdi_ftos.h"

#if CONF_WITH_VDI_OUTLINE_FONTS

#define FONT_DIR        "C:\\GEMSYS\\FONTS"

#define MAX_FACES       1000    /* faces put on the workstations */
#define MAX_VALUE       32000L  /* the largest size or metric kept */
#define HEIGHT_STEPS    8       /* to find the size of a character height */

/* the effects the engine draws: all that the VDI has */
#define ENGINE_EFFECTS  (F_THICKEN | F_LIGHT | F_SKEW | F_UNDER | F_OUTLINE)

/* an outline face, as vqt_name() and vst_font() see it */
typedef struct {
    Fonthead head;              /* chained by next_font */
    VdiOutlineFont *face;       /* as registered with the engine */
} OutlineFace;

static OutlineFace *faces;
static WORD face_count;
static BOOL scanned;            /* the fonts folder has been scanned */
static VdiXfntInfo xfnt;        /* too large for the stack */

/*
 * The engine and FreeType run on a stack of their own: a VDI call comes
 * on the caller's supervisor stack, which under EmuTOS's own AES is the
 * process's AES stack (AES_STACK_SIZE), far too small for FreeType.
 * fVDI does the same, for the same reason.  Drawing and sizing the faces
 * of C:\GEMSYS\FONTS were measured to use 18.5 KB of it.
 */
#define FONT_STACK_SIZE 32768L  /* bytes */

static LONG font_stack[FONT_STACK_SIZE / sizeof(LONG)];
static BOOL font_stack_busy;


/*
 * ===========================================================================
 * Loading the fonts
 * ===========================================================================
 */

static void found_font(const char *path, void *ctx)
{
    UNUSED(ctx);

    /* a file that cannot be used adds no face, and the others go on */
    vdi_ft_register_file(path);
}


/* give each registered face a font header */
static void make_faces(void)
{
    LONG n = vdi_ft_face_count(), i;
    OutlineFace *f;

    if (n <= 0)
        return;
    if (n > MAX_FACES)
        n = MAX_FACES;

    faces = vdi_ft_alloc(n * sizeof(OutlineFace));
    if (!faces)
        return;
    bzero(faces, n * sizeof(OutlineFace));

    for (i = 0, f = faces; i < n; i++, f++)
    {
        VdiOutlineFont *face = vdi_ft_face(i);

        if (!face)
            break;
        f->face = face;
        f->head.font_id = (WORD)face->id;
        memcpy(f->head.name, face->name, FONT_NAME_LEN);
        f->head.name[FONT_NAME_LEN-1] = '\0';
        f->head.first_ade = (UWORD)face->first_ade;
        f->head.last_ade = (UWORD)face->last_ade;
        f->head.flags = F_OUTLINE_FONT;
        if (face->flags & VDI_FT_MONOSPACE)
            f->head.flags |= F_MONOSPACE;
        if (i > 0)
            f[-1].head.next_font = &f->head;
    }
    face_count = (WORD)i;
}


void outline_init_vwk(Vwk *vwk)
{
    bzero(&vwk->outline_head, sizeof(vwk->outline_head));
    vwk->outline_face = NULL;
    vwk->outline_font = NULL;
    vwk->outline_count = 0;
}


/* see outline_load_fonts() */
static WORD load_fonts(Vwk *vwk)
{
    if (!scanned)
    {
        if (vdi_ft_init() < 0)
            return 0;               /* no memory: maybe next time */
        vdi_font_dir_scan(FONT_DIR, found_font, NULL);
        make_faces();
        scanned = TRUE;
    }

    if (!vwk->outline_count)
    {
        vwk->outline_count = face_count;
        vwk->num_fonts += face_count;
    }

    return face_count;
}


void outline_unload_fonts(Vwk *vwk)
{
    if (outline_font_selected(vwk))
    {
        vwk->cur_font = def_font;
        vwk->scaled = FALSE;
    }
    vwk->outline_face = NULL;
    vwk->outline_font = NULL;
    vwk->num_fonts -= vwk->outline_count;
    vwk->outline_count = 0;
}


/* vst_font(): the face with this id, if the workstation has loaded it */
const Fonthead *outline_find_face(const Vwk *vwk, WORD id)
{
    WORD i;

    for (i = 0; i < vwk->outline_count; i++)
        if (faces[i].head.font_id == id)
            return &faces[i].head;

    return NULL;
}


/* vqt_name(): the nth face loaded by the workstation, from 1 */
const Fonthead *outline_face_number(const Vwk *vwk, WORD n)
{
    if ((n < 1) || (n > vwk->outline_count))
        return NULL;

    return &faces[n-1].head;
}


/*
 * ===========================================================================
 * Selecting a face and a size
 * ===========================================================================
 */

/*
 * the outline face that the current font is, or is a size of
 *
 * vst_font() makes the header of a face the current font, and then has
 * it sized by vst_point() or vst_height()
 */
static VdiOutlineFont *current_face(const Vwk *vwk)
{
    WORD i;

    if (vwk->cur_font == &vwk->outline_head)
        return vwk->outline_face;

    for (i = 0; i < face_count; i++)
        if (vwk->cur_font == &faces[i].head)
            return faces[i].face;

    return NULL;
}


BOOL outline_face_selected(const Vwk *vwk)
{
    return current_face(vwk) != NULL;
}


/* an outline font is selected, and sized */
BOOL outline_font_selected(const Vwk *vwk)
{
    return (vwk->cur_font == &vwk->outline_head) && vwk->outline_font;
}


/* the selected sized font, for the calls that use the engine directly */
VdiOutlineFont *outline_current(Vwk *vwk)
{
    return outline_font_selected(vwk) ? vwk->outline_font : NULL;
}


static UWORD metric(LONG value)
{
    if (value < 0)
        return 0;
    if (value > MAX_VALUE)
        return (UWORD)MAX_VALUE;

    return (UWORD)value;
}


static WORD clamp_word(LONG value)
{
    if (value > 32767L)
        return 32767;
    if (value < -32768L)
        return -32768;

    return (WORD)value;
}


/* make a sized font the current font, with its metrics in outline_head */
static void use_font(Vwk *vwk, VdiOutlineFont *face, VdiOutlineFont *f)
{
    Fonthead *h = &vwk->outline_head;

    bzero(h, sizeof(*h));
    h->font_id = (WORD)f->id;
    h->point = (WORD)metric(f->points);
    memcpy(h->name, f->name, FONT_NAME_LEN);
    h->name[FONT_NAME_LEN-1] = '\0';
    h->first_ade = (UWORD)f->first_ade;
    h->last_ade = (UWORD)f->last_ade;
    h->top = metric(f->top);
    h->ascent = metric(f->ascent);
    h->half = metric(f->half);
    h->descent = metric(f->descent);
    h->bottom = metric(f->bottom);
    h->max_char_width = metric(f->max_char_width);
    h->max_cell_width = metric(f->max_cell_width);
    h->ul_size = metric(f->underline_size);
    h->flags = F_OUTLINE_FONT;
    if (f->flags & VDI_FT_MONOSPACE)
        h->flags |= F_MONOSPACE;
    h->form_height = metric(f->pixel_height);

    vwk->outline_face = face;
    vwk->outline_font = f;
    vwk->cur_font = h;
    vwk->scaled = FALSE;
}


/* the vertical resolution of the screen, in dots per inch */
static LONG dpi(void)
{
    LONG d = (ysize > 0) ? 25400L / ysize : 72;

    return (d > 0) ? d : 72;
}


/* the size of the em in pixels at this many points, as FreeType rounds it */
static LONG em_pixels(LONG points)
{
    LONG px = (points * dpi() + 36) / 72;

    return (px > 0) ? px : 1;
}


/* the face at an em of this many pixels */
static VdiOutlineFont *size_by_em(VdiOutlineFont *face, LONG em)
{
    LONG d = dpi(), points = (em * 72 + d / 2) / d;

    return vdi_ft_size(face, (points > 0) ? points : 1, em, 0);
}


/*
 * vst_point(): the largest size of the face's list that is not larger
 * than asked for (the smallest, if they all are), as GDOS and fVDI do
 */
static LONG listed_point(VdiOutlineFont *face, LONG point)
{
    LONG best = 0;
    WORD i;

    xfnt.pt_cnt = 0;
    vdi_ft_xfntinfo(face, VDI_XFNT_SIZES, &xfnt);
    if ((xfnt.pt_cnt <= 0) || (xfnt.pt_cnt > 64))
        return point;

    for (i = 0; i < xfnt.pt_cnt; i++)
        if ((xfnt.pt_sizes[i] <= point) && (xfnt.pt_sizes[i] > best))
            best = xfnt.pt_sizes[i];
    if (!best)
    {
        best = xfnt.pt_sizes[0];
        for (i = 1; i < xfnt.pt_cnt; i++)
            if (xfnt.pt_sizes[i] < best)
                best = xfnt.pt_sizes[i];
    }

    return (best > 0) ? best : point;
}


/*
 * size the current outline face in points; FALSE if it cannot be used
 */
static BOOL set_point(Vwk *vwk, WORD point)
{
    VdiOutlineFont *face = current_face(vwk), *f;
    LONG pt;

    if (!face)
        return FALSE;

    pt = listed_point(face, (point > 0) ? point : 1);
    f = vdi_ft_size(face, pt, em_pixels(pt), 0);
    if (!f)
        return FALSE;

    use_font(vwk, face, f);
    return TRUE;
}


/*
 * size the current outline face for a character height (top line to
 * baseline) in pixels: the largest whose top is not higher; FALSE if
 * the face cannot be used
 *
 * The em is not known from the height until the face has been sized, so
 * this sizes it at the height, scales the em by what that gave, and then
 * steps a pixel at a time.  Each size tried stays with the engine.
 */
static BOOL set_height(Vwk *vwk, WORD height)
{
    VdiOutlineFont *face = current_face(vwk), *f, *g;
    LONG want = height, em;
    WORD i;

    if (!face)
        return FALSE;
    if (want < 1)
        want = 1;
    if (want > MAX_VALUE)
        want = MAX_VALUE;

    em = want;
    f = size_by_em(face, em);
    if (!f)
        return FALSE;

    if ((f->top != want) && (f->top > 0))
    {
        em = (want * want) / f->top;
        if (em < 1)
            em = 1;
        if (em > MAX_VALUE)
            em = MAX_VALUE;
        g = size_by_em(face, em);
        if (!g)
            return FALSE;
        f = g;
    }

    for (i = 0; (i < HEIGHT_STEPS) && (f->top > want) && (em > 1); i++)
    {
        g = size_by_em(face, em - 1);
        if (!g)
            break;
        em--;
        f = g;
    }

    for (i = 0; (i < HEIGHT_STEPS) && (f->top < want) && (em < MAX_VALUE); i++)
    {
        g = size_by_em(face, em + 1);
        if (!g || (g->top > want))
            break;
        em++;
        f = g;
    }

    use_font(vwk, face, f);
    return TRUE;
}


/*
 * ===========================================================================
 * Measuring
 * ===========================================================================
 */

static LONG effects_of(const Vwk *vwk)
{
    return vwk->style & ENGINE_EFFECTS;
}


/* vqt_extent(), v_gtext(): the width of a string, effects included */
static WORD text_width(Vwk *vwk, WORD count, const WORD *str)
{
    VdiOutlineFont *f = outline_current(vwk);
    LONG width;

    if (!f || (count <= 0))
        return 0;
    if (vdi_ft_text_extent(f, str, count, VDI_FT_MAP_ATARI, effects_of(vwk), &width, NULL) < 0)
        return 0;

    return clamp_word(width);
}


/* vqt_width(): the advance of a character */
static WORD char_width(Vwk *vwk, WORD ch)
{
    VdiOutlineFont *f = outline_current(vwk);
    LONG advance[4];

    if (!f)
        return 0;
    if (vdi_ft_char_advance(f, (UWORD)ch, VDI_FT_MAP_ATARI, effects_of(vwk), advance) < 0)
        return 0;

    return clamp_word(advance[0]);
}


/*
 * ===========================================================================
 * Drawing
 * ===========================================================================
 */

/*
 * draw a one-plane bitmap with its top left corner at x,y, clipped to the
 * screen and to the clip rectangle
 *
 * The bitmap goes to text_blt() as one character of a font whose form is
 * the bitmap.  The clipping is done here, with LONGs, so that text_blt()
 * sees only what is on the screen.
 */
static void blit(const Vwk *vwk, const UWORD *data, LONG wdwidth, LONG width, LONG height,
                 LONG x, LONG y)
{
    LONG xmin = 0, ymin = 0, xmax = xres, ymax = yres;
    LONG sx = 0, sy = 0;

    if (vwk->clip)
    {
        if (vwk->xmn_clip > xmin)
            xmin = vwk->xmn_clip;
        if (vwk->ymn_clip > ymin)
            ymin = vwk->ymn_clip;
        if (vwk->xmx_clip < xmax)
            xmax = vwk->xmx_clip;
        if (vwk->ymx_clip < ymax)
            ymax = vwk->ymx_clip;
    }

    if (x < xmin)
    {
        sx = xmin - x;
        width -= sx;
        x = xmin;
    }
    if (y < ymin)
    {
        sy = ymin - y;
        height -= sy;
        y = ymin;
    }
    if (x + width - 1 > xmax)
        width = xmax - x + 1;
    if (y + height - 1 > ymax)
        height = ymax - y + 1;
    if ((width <= 0) || (height <= 0))
        return;

    FBASE = data;
    FWIDTH = (WORD)(wdwidth * 2);
    SOURCEX = (WORD)sx;
    SOURCEY = (WORD)sy;
    DELX = (UWORD)width;
    DELY = (UWORD)height;
    DESTX = (WORD)x;
    DESTY = (WORD)y;

    /*
     * the engine has drawn the effects; what the last bitmap-font text
     * left for them must go too, as the 16-bit blit skews by LOFF+ROFF
     * whatever STYLE says
     */
    STYLE = 0;
    LOFF = 0;
    ROFF = 0;
    WEIGHT = 0;
    SCALE = 0;
    CHUP = 0;                   /* the bitmap is turned already */
    MONO = 0;
    CLIP = 0;                   /* clipped above */
    WRT_MODE = vwk->wrt_mode;
    TEXTFG = vwk->text_color;
    SCRTCHP = vwk->scrtchp;
    SCRPT2 = vwk->scrpt2;

    text_blt();
}


/*
 * turn a bitmap counterclockwise by 90, 180 or 270 degrees, into a new
 * one to give back to vdi_ft_free(); NULL if there is no memory
 */
static UWORD *turn(const VdiOutlineBitmap *bm, WORD angle, LONG *new_wdwidth)
{
    LONG w = bm->width, h = bm->height, nw, nh, nwd, x, y;
    UWORD *out;

    if (angle == 1800)
    {
        nw = w;
        nh = h;
    }
    else
    {
        nw = h;
        nh = w;
    }
    nwd = (nw + 15) >> 4;
    out = vdi_ft_alloc(nwd * 2 * nh);
    if (!out)
        return NULL;
    bzero(out, nwd * 2 * nh);

    for (y = 0; y < h; y++)
    {
        const UWORD *row = bm->data + y * bm->wdwidth;
        UWORD *p, bit;

        switch (angle)
        {
        case 900:       /* x,y goes to y,w-1-x */
            p = out + (w - 1) * nwd + (y >> 4);
            bit = 0x8000 >> (y & 15);
            for (x = 0; x < w; x++, p -= nwd)
                if (row[x >> 4] & (0x8000 >> (x & 15)))
                    *p |= bit;
            break;
        case 1800:      /* x,y goes to w-1-x,h-1-y */
            p = out + (h - 1 - y) * nwd;
            for (x = 0; x < w; x++)
            {
                LONG rx = w - 1 - x;

                if (row[x >> 4] & (0x8000 >> (x & 15)))
                    p[rx >> 4] |= 0x8000 >> (rx & 15);
            }
            break;
        default:        /* 2700: x,y goes to h-1-y,x */
            p = out + ((h - 1 - y) >> 4);
            bit = 0x8000 >> ((h - 1 - y) & 15);
            for (x = 0; x < w; x++, p += nwd)
                if (row[x >> 4] & (0x8000 >> (x & 15)))
                    *p |= bit;
            break;
        }
    }

    *new_wdwidth = nwd;
    return out;
}


/*
 * draw a string with its pen starting at u0,v0 from the reference point
 * x,y, measured along and across the text before it is rotated (v0 is
 * where the top line of the font goes); returns the pen advance
 *
 * Rotated text goes where the VDI puts rotated system-font text: a point
 * u,v away from the reference point goes to x+u,y+v at 0 degrees,
 * x+v,y-u at 90, x-u,y-v at 180 and x-v,y+u at 270.
 */
static LONG draw_run(const Vwk *vwk, VdiOutlineFont *f, const WORD *str, WORD count,
                     LONG x, LONG y, LONG u0, LONG v0)
{
    VdiOutlineBitmap bm;
    WORD angle = vwk->chup;
    LONG u, advance, wd;
    UWORD *turned;

    if (vdi_ft_render(f, str, count, VDI_FT_MAP_ATARI, effects_of(vwk), 0, &bm) < 0)
        return 0;
    advance = bm.advance_x;
    if (!bm.data)
        return advance;

    u = u0 - bm.origin_x;       /* where the bitmap starts along the text */

    if ((angle != 900) && (angle != 1800) && (angle != 2700))   /* 0 or 3600 */
    {
        blit(vwk, bm.data, bm.wdwidth, bm.width, bm.height, x + u, y + v0);
        vdi_ft_release(&bm);
        return advance;
    }

    turned = turn(&bm, angle, &wd);
    if (turned)
    {
        switch (angle)
        {
        case 900:
            blit(vwk, turned, wd, bm.height, bm.width, x + v0, y - u - bm.width + 1);
            break;
        case 1800:
            blit(vwk, turned, wd, bm.width, bm.height, x - u - bm.width + 1, y - v0 - bm.height + 1);
            break;
        default:
            blit(vwk, turned, wd, bm.height, bm.width, x - v0 - bm.height + 1, y + u);
            break;
        }
        vdi_ft_free(turned);
    }
    vdi_ft_release(&bm);

    return advance;
}


/*
 * v_gtext(), v_justified(): draw a string in the current outline font
 *
 * 'width' is the width of the text for the horizontal alignment, or
 * negative if it has to be measured.  Justified text is drawn a
 * character at a time, spaced as output_text() spaces bitmap fonts.
 */
static void draw_text(Vwk *vwk, WORD count, const WORD *str, WORD width, JUSTINFO *justified)
{
    VdiOutlineFont *f = outline_current(vwk);
    LONG x, y, delh, delv, pen, dx, dy;
    WORD j;

    if (!f || (count <= 0))
        return;

    if (width < 0)
        width = text_width(vwk, count, str);

    switch(vwk->h_align) {
    default:                /* normally case 0: left justified */
        delh = 0;
        break;
    case 1:
        delh = width / 2;
        break;
    case 2:
        delh = width;
        break;
    }

    /* the distance from the top line, as for bitmap fonts */
    switch(vwk->v_align) {
    default:                /* normally case 0: baseline */
        delv = f->top;
        break;
    case 1:
        delv = f->top - f->half;
        break;
    case 2:
        delv = f->top - f->ascent;
        break;
    case 3:
        delv = f->top + f->bottom;
        break;
    case 4:
        delv = f->top + f->descent;
        break;
    case 5:
        delv = 0;
        break;
    }

    x = PTSIN[0];
    y = PTSIN[1];

    if (!justified)
    {
        draw_run(vwk, f, str, count, x, y, -delh, -delv);
        return;
    }

    pen = 0;
    dx = 0;
    dy = 0;
    for (j = 0; j < count; j++)
    {
        pen += draw_run(vwk, f, &str[j], 1, x + dx, y + dy, pen - delh, -delv);

        dx += justified->charx;
        dy += justified->chary;
        if (justified->rmchar) {
            dx += justified->rmcharx;
            dy += justified->rmchary;
            justified->rmchar--;
        }
        if (str[j] == ' ') {
            dx += justified->wordx;
            dy += justified->wordy;
            if (justified->rmword) {
                dx += justified->rmwordx;
                dy += justified->rmwordy;
                justified->rmword--;
            }
        }
    }
}


/*
 * ===========================================================================
 * Calling the engine on the font stack
 * ===========================================================================
 */

/* call fn(arg) with the stack pointer at top, and return what it returns */
static LONG switch_stack(LONG (*fn)(void *), void *arg, void *top)
{
    register LONG ret __asm__("d0");

    __asm__ volatile
    ("move.l  %%sp,%%a1\n\t"      /* the caller's stack */
     "move.l  %3,%%sp\n\t"
     "move.l  %%a1,-(%%sp)\n\t"   /* kept on the new one */
     "move.l  %2,-(%%sp)\n\t"
     "jsr     (%1)\n\t"
     "addq.l  #4,%%sp\n\t"
     "move.l  (%%sp),%%sp"        /* back to the caller's */
    : "=r"(ret)                                 /* outputs */
    : "a"(fn), "r"(arg), "r"(top)               /* inputs */
    : "d1", "a0", "a1", "cc", "memory"          /* clobbered */
    );

    return ret;
}


/*
 * call fn(arg) on the font stack
 *
 * The VDI is not reentered while the font stack is in use, except under
 * a multitasking GEMDOS, when a program sleeps in a file read made by the
 * engine and another one draws text: that one stays on its own stack,
 * which such a system makes large enough.
 *
 * Whatever calls the engine (vdi_ft2.h) for a workstation goes through
 * here.
 */
LONG outline_on_stack(LONG (*fn)(void *), void *arg)
{
    LONG ret;

    if (font_stack_busy)
        return fn(arg);

    font_stack_busy = TRUE;
    ret = switch_stack(fn, arg, font_stack + FONT_STACK_SIZE / sizeof(LONG));
    font_stack_busy = FALSE;

    return ret;
}


/* the arguments of the calls below, through outline_on_stack() */
typedef struct {
    Vwk *vwk;
    const WORD *str;
    JUSTINFO *justified;
    WORD count;
    WORD value;                 /* a size, a width or a character */
} OutlineCall;

static LONG load_fonts_call(void *p)
{
    OutlineCall *c = p;

    return load_fonts(c->vwk);
}

static LONG set_point_call(void *p)
{
    OutlineCall *c = p;

    return set_point(c->vwk, c->value);
}

static LONG set_height_call(void *p)
{
    OutlineCall *c = p;

    return set_height(c->vwk, c->value);
}

static LONG text_width_call(void *p)
{
    OutlineCall *c = p;

    return text_width(c->vwk, c->count, c->str);
}

static LONG char_width_call(void *p)
{
    OutlineCall *c = p;

    return char_width(c->vwk, c->value);
}

static LONG draw_text_call(void *p)
{
    OutlineCall *c = p;

    draw_text(c->vwk, c->count, c->str, c->value, c->justified);
    return 0;
}


/*
 * vst_load_fonts(): the number of outline faces
 *
 * The fonts folder is scanned by the first call only.
 */
WORD outline_load_fonts(Vwk *vwk)
{
    OutlineCall c;

    c.vwk = vwk;
    return (WORD)outline_on_stack(load_fonts_call, &c);
}


/* vst_point(): size the current outline face; FALSE if it cannot be used */
BOOL outline_set_point(Vwk *vwk, WORD point)
{
    OutlineCall c;

    c.vwk = vwk;
    c.value = point;
    return (BOOL)outline_on_stack(set_point_call, &c);
}


/* vst_height(): the same for a character height */
BOOL outline_set_height(Vwk *vwk, WORD height)
{
    OutlineCall c;

    c.vwk = vwk;
    c.value = height;
    return (BOOL)outline_on_stack(set_height_call, &c);
}


/* vqt_extent(), v_justified(): the width of a string, effects included */
WORD outline_text_width(Vwk *vwk, WORD count, const WORD *str)
{
    OutlineCall c;

    c.vwk = vwk;
    c.count = count;
    c.str = str;
    return (WORD)outline_on_stack(text_width_call, &c);
}


/* vqt_width(): the advance of a character */
WORD outline_char_width(Vwk *vwk, WORD ch)
{
    OutlineCall c;

    c.vwk = vwk;
    c.value = ch;
    return (WORD)outline_on_stack(char_width_call, &c);
}


/* v_gtext(), v_justified(): draw a string in the current outline font */
void outline_text(Vwk *vwk, WORD count, const WORD *str, WORD width, JUSTINFO *justified)
{
    OutlineCall c;

    c.vwk = vwk;
    c.count = count;
    c.str = str;
    c.value = width;
    c.justified = justified;
    outline_on_stack(draw_text_call, &c);
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS */
