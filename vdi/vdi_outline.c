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
 * - The first vst_load_fonts() made by any workstation, or the first call
 *   that asks for an outline font by its id or name (outline_scan()),
 *   registers the font files of C:\GEMSYS\FONTS with the engine.  Each
 *   face gets a font header, flagged F_OUTLINE_FONT and without a bitmap,
 *   which vqt_name() and vst_font() find after the fonts of font_ring[].
 *   They are kept out of font_ring[] itself, which line-A programs walk,
 *   and which cannot grow.  Every workstation sees them, as under fVDI and
 *   NVDI: programs select their fonts before they call vst_load_fonts()
 *   (HighWire does), which then gives their number.
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
 * - The NVDI calls (vdi_nvdi.c) add arbitrary sizes, widths, character
 *   sets (Unicode for one) and skew to the selected font, and fonts found
 *   in more folders later on (outline_rescan()): those come after the
 *   others.
 *
 * This file is compiled with -mshort like the rest of the VDI, so the
 * engine is only called through vdi_ft2.h, and sizes are LONGs.
 */

#include "emutos.h"
#include "string.h"
#include "vdi_defs.h"
#include "vdistub.h"
#include "lineavars.h"
#include "tosvars.h"
#include "biosdefs.h"
#include "vdi_ft2.h"
#include "vdi_ftos.h"
#include "vdi_ftmem.h"

#if CONF_WITH_VDI_OUTLINE_FONTS

#define FONT_DIR        "C:\\GEMSYS\\FONTS"

#define MAX_FACES       1000    /* faces put on the workstations */
#define MAX_VALUE       32000L  /* the largest size or metric kept */
#define HEIGHT_STEPS    8       /* to find the size of a character height */
#define MAX_SCAN_DIRS   64      /* folders taken from one outline_rescan() */
#define SKEW_LIMIT      800     /* vst_skew(), either way: the engine's limit */
#define MAX_STRETCH     (32L << 16)     /* vst_setsize(): width to height, at most */
#define MIN_STRETCH     ((1L << 16) / 32)       /* and at least */

/* the codes vqt_fontinfo() gives for an outline font, by character set */
#define BICS_CHARS      564     /* BICS_COUNT in vdi_ftmap.h */
#define UNICODE_FIRST   0x0020  /* the codes the BICS characters have in Unicode */
#define UNICODE_LAST    0xfb04

/* the effects the engine draws: all that the VDI has */
#define ENGINE_EFFECTS  (F_THICKEN | F_LIGHT | F_SKEW | F_UNDER | F_OUTLINE)

/* the sizes asked for while the engine was busy (Vwk.outline_pending) */
#define PENDING_NONE    0
#define PENDING_POINT   1       /* vst_point() */
#define PENDING_HEIGHT  2       /* vst_height() */
#define PENDING_ARBPT   3       /* vst_arbpt() */

/* an outline face, as vqt_name() and vst_font() see it */
typedef struct {
    Fonthead head;              /* chained by next_font */
    VdiOutlineFont *face;       /* as registered with the engine */
} OutlineFace;

static OutlineFace **face_tab;  /* the faces, in the engine's order */
static WORD face_count;
static BOOL scanned;            /* the faces are ready */

/* the outline_font of a workstation whose face is still to be sized; not for the engine */
static VdiOutlineFont unsized;

/*
 * The engine and FreeType run on a stack of their own: a VDI call comes
 * on the caller's supervisor stack, which under EmuTOS's own AES is the
 * process's AES stack (AES_STACK_SIZE), far too small for FreeType.
 * fVDI does the same, for the same reason.  Drawing and sizing TrueType
 * faces was measured to use 18.5 KB of it, and a CFF glyph built of two
 * others (seac) about 35 KB.
 *
 * FreeType does not bound such nesting, so the allocator (vdi_ftmem.c)
 * gives nothing to a call that comes within VDI_FT_STACK_GUARD bytes of the
 * bottom (see vdi_ft2.h and outline_stack_low()).  Should a call still go
 * past the bottom, it overwrites the canary below it, which is checked
 * after every call: the engine is then turned off for good, as nothing it
 * holds can be trusted.  The flags are kept above the stack, out of an
 * overflow's reach, and the canary and a scratch buffer keep whatever the
 * linker puts below the stack (other variables of the VDI) at a distance.
 *
 * The top of the stack is on a multiple of 16 bytes (stack_top()), and the
 * functions on the way from switch_stack() to the engine take LONGs and
 * pointers, or words in pairs: a lone WORD argument would leave the stack
 * pointer at 2 mod 4, and every long that the engine pushes misaligned,
 * which costs a 68020 or later.
 *
 * The engine is not reentrant, and the font stack is used by one call at a
 * time.  Under EmuTOS's own GEMDOS nothing can come in while a call is on
 * it.  Under a multitasking GEMDOS, a program can sleep in a file read that
 * the engine makes, and another can then call the VDI: outline_on_stack()
 * lets the other programs run until the stack is free (wait_for_stack()),
 * and only then, or if that takes too long, says it is busy.  The calls
 * that draw or measure then use the system font, and those that size a
 * face keep it, to be sized by its next use (keep_pending()).
 *
 * Known limits:
 * - if a call never comes back (a program killed while it sleeps in the
 *   engine, or a fault in it), the font stack stays busy and the outline
 *   fonts are drawn in the system font until the next boot.  Clearing the
 *   flag then is not safe, as the engine may have been left halfway through
 *   a change to its caches;
 * - the state of a call that sleeps here (the parameter block, the current
 *   workstation, the first points of PTSIN) is kept and put back by
 *   vdi_sleep_begin() and vdi_sleep_end() around every OS call (see
 *   vdi_sleep.h).  A static that code off the font stack can set must not
 *   be set before a call into the engine and read after it, as another
 *   program's call can set it meanwhile.  One that only code on the font
 *   stack sets may be: listed_point() sets font_stack.xfnt and reads it
 *   across a call into the engine, and runs only on the font stack, which
 *   one call uses at a time.
 */
#define FONT_STACK_SIZE VDI_FT_STACK_SIZE   /* bytes */
#define CANARY_LONGS    64                  /* below the stack */
#define CANARY          0xf0e1d2c3L
#define BUSY_WAIT       (3 * CLOCKS_PER_SEC)    /* without progress, at most */

static struct {
    VdiXfntInfo xfnt;           /* listed_point()'s, too large for the stack */
    LONG canary[CANARY_LONGS];  /* never used, unless the stack overflows */
    LONG stack[FONT_STACK_SIZE / sizeof(LONG)];
    BOOL canary_set;
    BOOL busy;                  /* a call is on the stack */
    BOOL off;                   /* it went past the bottom: the engine is not called again */
    BOOL given_up;              /* a wait gave up, and the stack has not been free since */
    WORD waiting;               /* calls waiting for the stack */
    LONG files_found;           /* by the scans, to tell a busy stack that gets on */
} font_stack;


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
    font_stack.files_found++;
}


/*
 * give each face registered since the last call a font header
 *
 * The headers of the faces already there stay where they are: a
 * workstation's current font may be one of them.
 */
static void make_faces(void)
{
    LONG n = vdi_ft_face_count(), i;
    OutlineFace *f, **tab, **old;

    if (n > MAX_FACES)
        n = MAX_FACES;
    if (n <= face_count)
        return;

    /*
     * The calls below can sleep, and calls off the font stack read face_tab
     * meanwhile: the old table goes only once the new one is in place.
     */
    tab = vdi_ft_alloc(n * sizeof(OutlineFace *));
    if (!tab)
        return;
    old = face_tab;
    if (face_count)
        memcpy(tab, old, face_count * sizeof(OutlineFace *));
    face_tab = tab;
    vdi_ft_free(old);
    f = vdi_ft_alloc((n - face_count) * sizeof(OutlineFace));
    if (!f)
        return;
    bzero(f, (n - face_count) * sizeof(OutlineFace));

    for (i = face_count; i < n; i++, f++)
    {
        VdiOutlineFont *face = vdi_ft_face(i);

        if (!face)
            break;
        tab[i] = f;
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
            tab[i-1]->head.next_font = &f->head;
    }
    face_count = (WORD)i;
}


/* a workstation that has loaded the outline fonts counts those added since */
static void show_new_faces(Vwk *vwk)
{
    if (vwk->outline_count && (vwk->outline_count < face_count))
    {
        vwk->num_fonts += face_count - vwk->outline_count;
        vwk->outline_count = face_count;
    }
}


void outline_init_vwk(Vwk *vwk)
{
    bzero(&vwk->outline_head, sizeof(vwk->outline_head));
    vwk->outline_face = NULL;
    vwk->outline_font = NULL;
    vwk->outline_count = 0;
    vwk->outline_em = 0;
    vwk->outline_width = 0;
    vwk->outline_map = VDI_FT_MAP_ATARI;
    vwk->outline_skew = 0;
    vwk->outline_pending = PENDING_NONE;

    /* this VDI serves outline fonts: say so to who looks for NVDI */
    nvdi_add_cookie();
}


/*
 * scan the fonts folder, once: on the font stack, which one call uses at a
 * time, so that the scan is never made twice
 */
static void scan_fonts(void)
{
    if (scanned || (vdi_ft_init() < 0))
        return;                     /* no memory: maybe next time */
    vdi_font_dir_scan(FONT_DIR, found_font, NULL);
    make_faces();
    scanned = TRUE;
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
    vwk->outline_pending = PENDING_NONE;
    vwk->num_fonts -= vwk->outline_count;
    vwk->outline_count = 0;
}


/*
 * vst_font(): the face with this id; NULL if there is none
 *
 * As under fVDI and NVDI, every workstation sees the faces once the fonts
 * folder has been scanned, whether it called vst_load_fonts() or not:
 * programs select their fonts before (HighWire does).
 */
const Fonthead *outline_find_face(const Vwk *vwk, WORD id)
{
    WORD i;

    UNUSED(vwk);
    for (i = 0; i < face_count; i++)
        if (face_tab[i]->head.font_id == id)
            return &face_tab[i]->head;

    return NULL;
}


/* vqt_name(): the nth outline face, from 1 */
const Fonthead *outline_face_number(const Vwk *vwk, WORD n)
{
    UNUSED(vwk);
    if ((n < 1) || (n > face_count))
        return NULL;

    return &face_tab[n-1]->head;
}


/* the outline face whose header this is; NULL if it is no outline face */
VdiOutlineFont *outline_face_of(const Fonthead *head)
{
    WORD i;

    for (i = 0; i < face_count; i++)
        if (head == &face_tab[i]->head)
            return face_tab[i]->face;

    return NULL;
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
        if (vwk->cur_font == &face_tab[i]->head)
            return face_tab[i]->face;

    return NULL;
}


BOOL outline_face_selected(const Vwk *vwk)
{
    return current_face(vwk) != NULL;
}


/*
 * an outline font is selected, and sized, or to be sized by its next use
 * (keep_pending()); vdi_asm.S makes the same test
 */
BOOL outline_font_selected(const Vwk *vwk)
{
    return (vwk->cur_font == &vwk->outline_head) && vwk->outline_font;
}


/*
 * the selected sized font, for the calls that use the engine directly;
 * NULL while it is still to be sized
 */
VdiOutlineFont *outline_current(Vwk *vwk)
{
    if (!outline_font_selected(vwk) || (vwk->outline_font == &unsized))
        return NULL;

    return vwk->outline_font;
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


/* the codes of an outline font in a character set, for vqt_fontinfo() */
static void set_char_range(Fonthead *h, WORD map)
{
    switch(map) {
    case VDI_FT_MAP_BICS:
        h->first_ade = 0;
        h->last_ade = BICS_CHARS;
        break;
    case VDI_FT_MAP_UNICODE:
        h->first_ade = UNICODE_FIRST;
        h->last_ade = UNICODE_LAST;
        break;
    default:
        h->first_ade = 0;
        h->last_ade = 255;
        break;
    }
}


/*
 * make a sized font the current font, with its metrics in outline_head;
 * em is the size of its em in pixels, width the character width it was
 * made with (0: natural)
 */
static void use_font(Vwk *vwk, VdiOutlineFont *face, VdiOutlineFont *f, LONG em, LONG width)
{
    Fonthead *h = &vwk->outline_head;

    bzero(h, sizeof(*h));
    h->font_id = (WORD)f->id;
    h->point = (WORD)metric(f->points);
    memcpy(h->name, f->name, FONT_NAME_LEN);
    h->name[FONT_NAME_LEN-1] = '\0';
    set_char_range(h, vwk->outline_map);
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
    vwk->outline_em = (WORD)em;
    vwk->outline_width = (WORD)width;
    vwk->outline_pending = PENDING_NONE;
    vwk->cur_font = h;
    vwk->scaled = FALSE;
}


/*
 * the engine is busy with another program's call: keep the current face
 * selected, to be sized as asked (kind, value) by the next call that uses
 * it (sized_font()).  Until then it has the metrics of the system font,
 * which its text is drawn in while the engine stays busy.
 *
 * Returns FALSE if no outline face is selected.
 */
static BOOL keep_pending(Vwk *vwk, WORD kind, WORD value)
{
    VdiOutlineFont *face = current_face(vwk);
    const Fonthead *sys = def_font;
    Fonthead *h = &vwk->outline_head;

    if (!face)
        return FALSE;

    bzero(h, sizeof(*h));
    h->font_id = (WORD)face->id;
    h->point = (kind == PENDING_HEIGHT) ? sys->point : value;
    memcpy(h->name, face->name, FONT_NAME_LEN);
    h->name[FONT_NAME_LEN-1] = '\0';
    set_char_range(h, vwk->outline_map);
    h->top = sys->top;
    h->ascent = sys->ascent;
    h->half = sys->half;
    h->descent = sys->descent;
    h->bottom = sys->bottom;
    h->max_char_width = sys->max_char_width;
    h->max_cell_width = sys->max_cell_width;
    h->ul_size = sys->ul_size;
    h->flags = F_OUTLINE_FONT;
    if (face->flags & VDI_FT_MONOSPACE)
        h->flags |= F_MONOSPACE;
    h->form_height = sys->form_height;

    vwk->outline_face = face;
    vwk->outline_font = &unsized;
    vwk->outline_em = 0;
    vwk->outline_width = 0;
    vwk->outline_pending = kind;
    vwk->outline_asked = value;
    vwk->cur_font = h;
    vwk->scaled = FALSE;

    return TRUE;
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

    if (px > MAX_VALUE)
        px = MAX_VALUE;

    return (px > 0) ? px : 1;
}


/* the face at an em of this many pixels */
static VdiOutlineFont *size_by_em(VdiOutlineFont *face, LONG em)
{
    LONG d = dpi(), points = (em * 72 + d / 2) / d;

    return vdi_ft_size(face, (points > 0) ? points : 1, em, 0);
}


static VdiOutlineFont *sized_font(Vwk *vwk);


/*
 * vst_point(): the largest size of the face's list that is not larger
 * than asked for (the smallest, if they all are), as GDOS and fVDI do
 */
static LONG listed_point(VdiOutlineFont *face, LONG point)
{
    VdiXfntInfo *xfnt = &font_stack.xfnt;
    LONG best = 0;
    WORD i;

    xfnt->pt_cnt = 0;
    vdi_ft_xfntinfo(face, VDI_XFNT_SIZES, xfnt);
    if ((xfnt->pt_cnt <= 0) || (xfnt->pt_cnt > 64))
        return point;

    for (i = 0; i < xfnt->pt_cnt; i++)
        if ((xfnt->pt_sizes[i] <= point) && (xfnt->pt_sizes[i] > best))
            best = xfnt->pt_sizes[i];
    if (!best)
    {
        best = xfnt->pt_sizes[0];
        for (i = 1; i < xfnt->pt_cnt; i++)
            if (xfnt->pt_sizes[i] < best)
                best = xfnt->pt_sizes[i];
    }

    return (best > 0) ? best : point;
}


/*
 * size the current outline face in points; FALSE if it cannot be used
 */
static BOOL set_point(Vwk *vwk, LONG point)
{
    VdiOutlineFont *face = current_face(vwk), *f;
    LONG pt;

    if (!face)
        return FALSE;

    pt = listed_point(face, (point > 0) ? point : 1);
    f = vdi_ft_size(face, pt, em_pixels(pt), 0);
    if (!f)
        return FALSE;

    use_font(vwk, face, f, em_pixels(pt), 0);
    return TRUE;
}


/*
 * vst_arbpt(): size the current outline face at any size in points, not
 * only those of its list; FALSE if it cannot be used
 */
static BOOL set_arbpt(Vwk *vwk, LONG point)
{
    VdiOutlineFont *face = current_face(vwk), *f;
    LONG em;

    if (!face)
        return FALSE;
    if (point < 1)
        point = 1;
    if (point > MAX_VALUE)
        point = MAX_VALUE;

    em = em_pixels(point);
    f = vdi_ft_size(face, point, em, 0);
    if (!f)
        return FALSE;

    use_font(vwk, face, f, em, 0);
    return TRUE;
}


/*
 * vst_width(): the selected size with its widest character this many
 * pixels wide, and its height unchanged (as NVDI); FALSE if that cannot be
 */
static BOOL set_width(Vwk *vwk, LONG width)
{
    VdiOutlineFont *f = sized_font(vwk), *g;

    if (!f || !(f->flags & VDI_FT_SCALABLE) || (width <= 0) || (width > MAX_VALUE))
        return FALSE;

    g = vdi_ft_size(vwk->outline_face, f->points, vwk->outline_em, width);
    if (!g)
        return FALSE;

    use_font(vwk, vwk->outline_face, g, vwk->outline_em, width);
    return TRUE;
}


/*
 * vst_setsize(): the selected size made as wide as a size of this many
 * points (in 1/65536 points), its height unchanged; FALSE if that cannot be
 */
static BOOL set_size(Vwk *vwk, LONG width)
{
    VdiOutlineFont *f = sized_font(vwk), *natural, *g;
    LONG ratio, chars;

    if (!f || !(f->flags & VDI_FT_SCALABLE) || (width <= 0) || (f->points <= 0))
        return FALSE;

    natural = vdi_ft_size(vwk->outline_face, f->points, vwk->outline_em, 0);
    if (!natural)
        return FALSE;

    /* width to height, in 1/65536, then the widest character at that */
    ratio = width / f->points;
    if (ratio > MAX_STRETCH)
        ratio = MAX_STRETCH;
    if (ratio < MIN_STRETCH)
        ratio = MIN_STRETCH;
    chars = (LONG)(((ULONG)natural->max_char_width * (ULONG)(ratio >> 5) + 1024) >> 11);
    if (chars < 1)
        chars = 1;
    if (chars > MAX_VALUE)
        chars = MAX_VALUE;
    if (chars == natural->max_char_width)
        chars = 0;                  /* that is the natural width */

    g = chars ? vdi_ft_size(vwk->outline_face, f->points, vwk->outline_em, chars) : natural;
    if (!g)
        return FALSE;

    use_font(vwk, vwk->outline_face, g, vwk->outline_em, chars);
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
static BOOL set_height(Vwk *vwk, LONG height)
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

    use_font(vwk, face, f, em, 0);
    return TRUE;
}


/*
 * on the font stack: the selected sized font, made first if it was asked
 * for while the engine was busy (keep_pending()); NULL if there is none,
 * or if the engine cannot make it now
 */
static VdiOutlineFont *sized_font(Vwk *vwk)
{
    if ((vwk->outline_font == &unsized) && (vwk->cur_font == &vwk->outline_head))
    {
        switch(vwk->outline_pending) {
        case PENDING_POINT:
            set_point(vwk, vwk->outline_asked);
            break;
        case PENDING_HEIGHT:
            set_height(vwk, vwk->outline_asked);
            break;
        default:
            set_arbpt(vwk, vwk->outline_asked);
            break;
        }
    }

    return outline_current(vwk);
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


/*
 * vqt_extent(), v_gtext(): the width of a string, effects included;
 * negative if the engine cannot measure it
 */
static LONG text_width(Vwk *vwk, LONG count, const WORD *str)
{
    VdiOutlineFont *f = sized_font(vwk);
    LONG width;

    if (!f)
        return -1;
    if (count <= 0)
        return 0;
    if (vdi_ft_text_extent(f, str, count, vwk->outline_map, effects_of(vwk), &width, NULL) < 0)
        return -1;

    return clamp_word(width);
}


/* vqt_width(): the advance of a character; negative if the engine cannot tell */
static LONG char_width(Vwk *vwk, LONG ch)
{
    VdiOutlineFont *f = sized_font(vwk);
    LONG advance[4];

    if (!f)
        return -1;
    if (vdi_ft_char_advance(f, (UWORD)ch, vwk->outline_map, effects_of(vwk), advance) < 0)
        return -1;

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
#if CONF_WITH_VDI_NVDI_EXT && CONF_WITH_VDI_16BIT
    vdi_bg_text = TRUECOLOR_MODE ? vdi_bg_index(vwk, VDI_COL_TEXT) : 0;
#endif
    SCRTCHP = vwk->scrtchp;
    SCRPT2 = vwk->scrpt2;

    text_blt();
}


/*
 * turn a bitmap counterclockwise by 90, 180 or 270 degrees, into a new
 * one to give back to vdi_ft_free(); NULL if there is no memory
 */
static UWORD *turn(const VdiOutlineBitmap *bm, LONG angle, LONG *new_wdwidth)
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
 *
 * Returns -1, having drawn nothing, if the engine cannot render the
 * string or there is no memory to turn it.
 */
static LONG draw_run(const Vwk *vwk, VdiOutlineFont *f, const WORD *str, LONG count,
                     LONG x, LONG y, LONG u0, LONG v0)
{
    VdiOutlineBitmap bm;
    WORD angle = vwk->chup;
    LONG u, advance, wd;
    UWORD *turned;

    if (vdi_ft_render(f, str, count, vwk->outline_map, effects_of(vwk), vwk->outline_skew, &bm) < 0)
        return -1;
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
    if (!turned)
        advance = -1;
    else
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
 * v_gtext(), v_justified(): draw a string in the current outline font,
 * with its reference point at x,y
 *
 * 'width' is the width of the text for the horizontal alignment, or
 * negative if it has to be measured.  Justified text is drawn a
 * character at a time, spaced as output_text() spaces bitmap fonts.
 *
 * Returns FALSE, having drawn nothing, if the engine cannot draw it.
 */
static BOOL draw_text(Vwk *vwk, LONG count, const WORD *str, LONG width, JUSTINFO *justified,
                      LONG x, LONG y)
{
    VdiOutlineFont *f = sized_font(vwk);
    LONG delh, delv, pen, dx, dy, advance;
    WORD j;

    if (!f)
        return FALSE;
    if (count <= 0)
        return TRUE;

    if (width < 0)
    {
        LONG w = text_width(vwk, count, str);

        if (w < 0)
            return FALSE;
        width = (WORD)w;
    }

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

    if (!justified)
        return draw_run(vwk, f, str, count, x, y, -delh, -delv) >= 0;

    pen = 0;
    dx = 0;
    dy = 0;
    for (j = 0; j < count; j++)
    {
        advance = draw_run(vwk, f, &str[j], 1, x + dx, y + dy, pen - delh, -delv);
        if (advance < 0)
        {
            if (j == 0)
                return FALSE;   /* nothing drawn yet: all in the system font */
            advance = 0;        /* else leave out what cannot be drawn */
        }
        pen += advance;

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

    return TRUE;
}


/*
 * ===========================================================================
 * Calling the engine on the font stack
 * ===========================================================================
 */

/*
 * call fn(arg) with the stack pointer at top, and return what it returns;
 * the caller's stack pointer is kept at top - 4 (outline_caller_stack())
 */
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
 * the top of the font stack, on a multiple of 16 bytes: the linker keeps
 * the variables of EmuTOS only 2-aligned, and a 68020 or later is slower
 * with longs that are not aligned
 */
static LONG *stack_top(void)
{
    return (LONG *)((ULONG)(font_stack.stack + ARRAY_SIZE(font_stack.stack)) & ~15UL);
}


/*
 * TRUE when the call on the font stack has come within VDI_FT_STACK_GUARD
 * bytes of its bottom, where vdi_ft_alloc() gives no more memory; FALSE
 * off the font stack
 */
BOOL outline_stack_low(void)
{
    char here;
    ULONG sp = (ULONG)&here;

    return (sp >= (ULONG)font_stack.canary)
        && (sp < (ULONG)font_stack.stack + VDI_FT_STACK_GUARD);
}


/*
 * the stack pointer the call on the font stack had before it came there,
 * kept at the top of the font stack by switch_stack(); NULL off the font
 * stack.  The OS glue makes the OS calls that can sleep there (see
 * vdi_ftos.c).
 */
void *outline_caller_stack(void)
{
    char here;
    ULONG sp = (ULONG)&here;

    if ((sp < (ULONG)font_stack.canary)
     || (sp >= (ULONG)stack_top()))
        return NULL;

    return (void *)stack_top()[-1];
}


/* the canary below the font stack is as it was put there */
static BOOL canary_intact(void)
{
    WORD i;

    for (i = 0; i < CANARY_LONGS; i++)
        if (font_stack.canary[i] != CANARY)
            return FALSE;

    return TRUE;
}


/*
 * the font stack is busy: let the other programs run until it is free;
 * TRUE once it is, FALSE if it is not after BUSY_WAIT, or with no other
 * program that could free it (a single-tasking GEMDOS), or once *done
 * (if done is not NULL) says that what was waited for is there anyway
 *
 * The time counts from the last font file a scan on the stack has found,
 * as a scan can take long.  Once a wait has given up, the next ones give
 * up at once until the stack has been free again: a call that never comes
 * back (see the known limits above) must not make every later call wait.
 */
static BOOL wait_for_stack(const BOOL *done)
{
    LONG start, found;
    BOOL got_it = TRUE;

    if (font_stack.given_up)
        return FALSE;

    font_stack.waiting++;
    start = hz_200;
    found = font_stack.files_found;
    while (font_stack.busy)
    {
        if (font_stack.files_found != found)
        {
            found = font_stack.files_found;
            start = hz_200;
        }
        if (done && *done)
        {
            got_it = FALSE;
            break;
        }
        if ((hz_200 - start > (LONG)BUSY_WAIT) || font_stack.off || !vdi_yield())
        {
            font_stack.given_up = TRUE;
            got_it = FALSE;
            break;
        }
    }
    font_stack.waiting--;

    return got_it;
}


/*
 * call fn(arg) on the font stack, and put what it returns in *result;
 * returns OUTLINE_DONE
 *
 * Returns OUTLINE_BUSY, without calling fn, if another program's call
 * keeps the stack after wait_for_stack(), and OUTLINE_FAILED if the
 * engine has been turned off (or is turned off by this call, which went
 * past the bottom of the stack).  Whatever calls the engine (vdi_ft2.h)
 * for a workstation goes through here.
 */
static WORD on_stack(LONG (*fn)(void *), void *arg, LONG *result, const BOOL *done)
{
    LONG ret;
    WORD i;

    if (font_stack.off)
        return OUTLINE_FAILED;
    if (font_stack.busy && !wait_for_stack(done))
        return font_stack.off ? OUTLINE_FAILED : OUTLINE_BUSY;

    if (!font_stack.canary_set)
    {
        for (i = 0; i < CANARY_LONGS; i++)
            font_stack.canary[i] = CANARY;
        font_stack.canary_set = TRUE;
    }

    font_stack.busy = TRUE;
    ret = switch_stack(fn, arg, stack_top());
    if (!canary_intact())
    {
        font_stack.off = TRUE;      /* and busy stays set */
        return OUTLINE_FAILED;
    }
    font_stack.busy = FALSE;
    font_stack.given_up = FALSE;
    if (font_stack.waiting)
        vdi_yield();                /* let a call that waits have it now */

    if (result)
        *result = ret;
    return OUTLINE_DONE;
}

WORD outline_on_stack(LONG (*fn)(void *), void *arg, LONG *result)
{
    return on_stack(fn, arg, result, NULL);
}


/* the arguments of the calls below, through outline_on_stack() */
typedef struct {
    Vwk *vwk;
    const WORD *str;
    JUSTINFO *justified;
    LONG x, y;                  /* the reference point of the text */
    WORD count;
    WORD value;                 /* a size, a width or a character */
    LONG a, b;                  /* more numbers */
    void *p, *q;                /* and pointers */
} OutlineCall;

static LONG scan_fonts_call(void *p)
{
    UNUSED(p);
    scan_fonts();
    return 0;
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

    return draw_text(c->vwk, c->count, c->str, c->value, c->justified, c->x, c->y);
}

static LONG set_arbpt_call(void *p)
{
    OutlineCall *c = p;

    return set_arbpt(c->vwk, c->a);
}

static LONG set_width_call(void *p)
{
    OutlineCall *c = p;

    return set_width(c->vwk, c->a);
}

static LONG set_size_call(void *p)
{
    OutlineCall *c = p;

    return set_size(c->vwk, c->a);
}

static LONG char_index_call(void *p)
{
    OutlineCall *c = p;
    VdiOutlineFont *f = sized_font(c->vwk);

    return f ? vdi_ft_char_index(f, (UWORD)c->value, c->a, c->b) : 0xffffL;
}

static LONG get_outline_call(void *p)
{
    OutlineCall *c = p;
    VdiOutlineFont *f = sized_font(c->vwk);

    if (!f)
        return 0;

    return vdi_ft_get_outline(f, (UWORD)c->value, c->vwk->outline_map, c->p, c->q, c->a);
}

static LONG xfntinfo_call(void *p)
{
    OutlineCall *c = p;

    vdi_ft_xfntinfo(c->p, c->a, c->q);
    return 0;
}

static LONG fontheader_call(void *p)
{
    OutlineCall *c = p;
    VdiOutlineFont *face = current_face(c->vwk);

    if (!face)
        return FALSE;
    vdi_ft_fontheader(face, c->p);
    return TRUE;
}

/*
 * scan more folders, which can be in the fonts folder or anywhere, for
 * fonts that are not yet there; returns the number of faces added
 */
static LONG rescan_call(void *p)
{
    OutlineCall *c = p;
    char **dirs = c->p;
    LONG count = c->a, i;
    WORD before, handle;
    Vwk *vwk;

    scan_fonts();                   /* the first scan, of the fonts folder */
    if (!scanned || !dirs)
        return 0;

    before = face_count;
    if (count > MAX_SCAN_DIRS)
        count = MAX_SCAN_DIRS;
    for (i = 0; i < count; i++)
        if (dirs[i] && dirs[i][0])
            vdi_font_dir_scan(dirs[i], found_font, NULL);  /* registered once only */
    make_faces();

    for (handle = 1; handle <= NUM_VDI_HANDLES; handle++)
        if ((vwk = get_vwk_by_handle(handle)) != NULL)
            show_new_faces(vwk);

    return face_count - before;
}


/*
 * the first call scans the fonts folder: the first vst_load_fonts(), or
 * the first call that asks for an outline font by its id or name.  Returns
 * OUTLINE_DONE once the faces are ready, OUTLINE_BUSY if another program's
 * call kept the engine too long (that may be the scan, which is waited for
 * as long as it finds files), OUTLINE_FAILED if there is no memory.
 *
 * Not from a process the MiNT kernel started itself (XaAES's AES thread):
 * walking the folder through GEMDOS from there makes XaAES fail (see
 * vdi_caller_is_system()).  A later call from a program does the scan.
 */
WORD outline_scan(void)
{
    WORD ret;

    if (scanned)
        return OUTLINE_DONE;
    if (vdi_caller_is_system())
        return OUTLINE_FAILED;

    ret = on_stack(scan_fonts_call, NULL, NULL, &scanned);  /* no wait once another call has scanned */
    if (scanned)
        ret = OUTLINE_DONE;
    else if (ret == OUTLINE_DONE)
        ret = OUTLINE_FAILED;

    return ret;
}


/*
 * vst_load_fonts(): the number of outline faces
 *
 * The fonts folder is scanned by the first call only.
 */
WORD outline_load_fonts(Vwk *vwk)
{
    if (outline_scan() != OUTLINE_DONE)
        return 0;                   /* no memory, or the engine stayed busy */

    if (!vwk->outline_count)
    {
        vwk->outline_count = face_count;
        vwk->num_fonts += face_count;
    }
    show_new_faces(vwk);

    return face_count;
}


/*
 * size the current face with fn(c) on the font stack; returns OUTLINE_DONE,
 * OUTLINE_BUSY (the face stays, to be sized as asked, kind and value, by
 * its next use) or OUTLINE_FAILED (the face cannot be used)
 */
static WORD size_face(Vwk *vwk, LONG (*fn)(void *), OutlineCall *c, WORD kind, WORD value)
{
    LONG ok = FALSE;
    WORD ret = outline_on_stack(fn, c, &ok);

    if ((ret == OUTLINE_BUSY) && keep_pending(vwk, kind, value))
        return OUTLINE_BUSY;
    if ((ret == OUTLINE_DONE) && ok)
        return OUTLINE_DONE;

    vwk->outline_pending = PENDING_NONE;
    return OUTLINE_FAILED;
}


/* vst_point(): size the current outline face */
WORD outline_set_point(Vwk *vwk, WORD point)
{
    OutlineCall c;

    c.vwk = vwk;
    c.value = point;
    return size_face(vwk, set_point_call, &c, PENDING_POINT, (point > 0) ? point : 1);
}


/* vst_height(): the same for a character height */
WORD outline_set_height(Vwk *vwk, WORD height)
{
    OutlineCall c;

    c.vwk = vwk;
    c.value = height;
    return size_face(vwk, set_height_call, &c, PENDING_HEIGHT, height);
}


static LONG size_pending_call(void *p)
{
    sized_font(p);
    return 0;
}


/*
 * vqt_fontinfo(), vqt_attributes(): a face kept while the engine was busy
 * (keep_pending()) is sized now, if the engine can, for its metrics
 */
void outline_size_pending(Vwk *vwk)
{
    if ((vwk->cur_font == &vwk->outline_head) && (vwk->outline_font == &unsized))
        outline_on_stack(size_pending_call, vwk, NULL);
}


/*
 * vqt_extent(), v_justified(): the width of a string, effects included;
 * FALSE if the engine cannot measure it now, for the system font to
 */
BOOL outline_text_width(Vwk *vwk, WORD count, const WORD *str, WORD *width)
{
    OutlineCall c;
    LONG w;

    c.vwk = vwk;
    c.count = count;
    c.str = str;
    if ((outline_on_stack(text_width_call, &c, &w) != OUTLINE_DONE) || (w < 0))
        return FALSE;

    *width = (WORD)w;
    return TRUE;
}


/* vqt_width(): the advance of a character; FALSE as above */
BOOL outline_char_width(Vwk *vwk, WORD ch, WORD *width)
{
    OutlineCall c;
    LONG w;

    c.vwk = vwk;
    c.value = ch;
    if ((outline_on_stack(char_width_call, &c, &w) != OUTLINE_DONE) || (w < 0))
        return FALSE;

    *width = (WORD)w;
    return TRUE;
}


/*
 * v_gtext(), v_justified(): draw a string in the current outline font at
 * PTSIN[0],PTSIN[1]; FALSE, having drawn nothing, if the engine cannot
 * draw it now, for the system font to
 */
BOOL outline_text(Vwk *vwk, WORD count, const WORD *str, WORD width, JUSTINFO *justified)
{
    OutlineCall c;
    LONG ok;

    c.vwk = vwk;
    c.count = count;
    c.str = str;
    c.value = width;
    c.justified = justified;
    c.x = PTSIN[0];             /* before anything can sleep */
    c.y = PTSIN[1];
    return (outline_on_stack(draw_text_call, &c, &ok) == OUTLINE_DONE) && ok;
}


/*
 * ===========================================================================
 * For the NVDI calls (vdi_nvdi.c)
 * ===========================================================================
 */

/* vst_arbpt(): size the current outline face in points, as outline_set_point() */
WORD outline_set_arbpt(Vwk *vwk, LONG point)
{
    OutlineCall c;

    if (point < 1)
        point = 1;
    if (point > MAX_VALUE)
        point = MAX_VALUE;

    c.vwk = vwk;
    c.a = point;
    return size_face(vwk, set_arbpt_call, &c, PENDING_ARBPT, (WORD)point);
}


/* vst_width(): the character width of the selected size; FALSE if unchanged */
BOOL outline_set_width(Vwk *vwk, WORD width)
{
    OutlineCall c;
    LONG ok;

    c.vwk = vwk;
    c.a = width;
    return (outline_on_stack(set_width_call, &c, &ok) == OUTLINE_DONE) && ok;
}


/* vst_setsize(): the width of the selected size, in 1/65536 points; FALSE if unchanged */
BOOL outline_set_size(Vwk *vwk, LONG width)
{
    OutlineCall c;
    LONG ok;

    c.vwk = vwk;
    c.a = width;
    return (outline_on_stack(set_size_call, &c, &ok) == OUTLINE_DONE) && ok;
}


/*
 * vst_charmap(), vst_map_mode(): the character set of the text; returns
 * the one set, which is always the Atari set unless an outline font is
 * selected (as fVDI)
 */
WORD outline_set_map(Vwk *vwk, WORD map)
{
    if ((map != VDI_FT_MAP_ATARI) && !outline_face_selected(vwk))
        map = VDI_FT_MAP_ATARI;

    vwk->outline_map = map;
    if (outline_font_selected(vwk))
        set_char_range(&vwk->outline_head, map);

    return map;
}


/* vst_skew(): the slant of outline text, in tenths of a degree; returns it */
WORD outline_set_skew(Vwk *vwk, WORD skew)
{
    if (skew > SKEW_LIMIT)
        skew = SKEW_LIMIT;
    if (skew < -SKEW_LIMIT)
        skew = -SKEW_LIMIT;

    return vwk->outline_skew = skew;
}


/* vqt_char_index(): a code from one character set to another, 0xffff if none */
WORD outline_char_index(Vwk *vwk, UWORD ch, WORD from_map, WORD to_map)
{
    OutlineCall c;
    LONG index;

    c.vwk = vwk;
    c.value = ch;
    c.a = from_map;
    c.b = to_map;
    if (outline_on_stack(char_index_call, &c, &index) != OUTLINE_DONE)
        return -1;

    return (WORD)index;
}


/*
 * v_getoutline(): a character's outline as v_bez points in pixels, y up
 * from the baseline; returns their number, 0 if none, -1 if more than
 * max_points
 */
WORD outline_get_outline(Vwk *vwk, UWORD ch, WORD *xy, BYTE *flags, WORD max_points)
{
    OutlineCall c;
    LONG n;

    c.vwk = vwk;
    c.value = ch;
    c.p = xy;
    c.q = flags;
    c.a = max_points;
    if (outline_on_stack(get_outline_call, &c, &n) != OUTLINE_DONE)
        return 0;

    return (WORD)n;
}


/* vqt_xfntinfo(): what the engine knows of a face; FALSE if it cannot tell now */
BOOL outline_xfntinfo(VdiOutlineFont *face, WORD flags, VdiXfntInfo *info)
{
    OutlineCall c;

    c.p = face;
    c.q = info;
    c.a = (UWORD)flags;
    return outline_on_stack(xfntinfo_call, &c, NULL) == OUTLINE_DONE;
}


/* vqt_fontheader(): the header of the current outline face; FALSE if none or not now */
BOOL outline_fontheader(Vwk *vwk, VdiFontHeader *fhdr)
{
    OutlineCall c;
    LONG ok;

    c.vwk = vwk;
    c.p = fhdr;
    return (outline_on_stack(fontheader_call, &c, &ok) == OUTLINE_DONE) && ok;
}


/*
 * vq_vgdos(): FALSE once the fonts folder is known to hold no font; until
 * the first vst_load_fonts() looks, there may be some
 */
BOOL outline_fonts_present(void)
{
    return !(scanned && (face_count == 0));
}


/*
 * NVDI's scan_fonts: the fonts in these folders that are not there yet are
 * added after the others; returns their number
 */
LONG outline_rescan(char **dirs, LONG count)
{
    OutlineCall c;
    LONG added;

    if (vdi_caller_is_system())     /* no folder walks from there */
        return 0;

    c.p = dirs;
    c.a = count;
    if (outline_on_stack(rescan_call, &c, &added) != OUTLINE_DONE)
        return 0;

    return added;
}


/* NVDI's get_font_dir: the fonts folder, without a trailing backslash */
const char *outline_font_dir(void)
{
    return FONT_DIR;
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS */
