/*
 * vdi_nvdi.c - the NVDI and SpeedoGDOS text calls, the GDOS answer and
 *              the NVDI cookie
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * With outline fonts, the VDI answers what programs ask of a vector font
 * GDOS (SpeedoGDOS, NVDI 3 and later):
 *
 * - vq_vgdos() (trap #2 with d0 = -2) answers '_FSM', see vdi_gdos_version();
 * - the text calls of those GDOSes (opcodes 190 and 229-253), as fVDI
 *   answers them; they use the outline fonts of vdi_outline.c, which calls
 *   the engine;
 * - an NVDI cookie, version 5.01, which NVDI-aware programs take as a
 *   pointer into NVDI's own data: it is padded with zeroes so that the
 *   offsets they read are inside it, and its scan_fonts (+$AC) and
 *   get_font_dir (+$B0) vectors work, so that a program can add a font
 *   file at run time (a web browser does, for downloaded fonts).
 *
 * The cookie is added by the first workstation this VDI opens, and only if
 * there is no NVDI cookie yet: when another VDI (fVDI, NVDI) has taken
 * over, it has its own, and EmuTOS's VDI does not serve the calls.
 *
 * Compiled with -mshort like the rest of the VDI: the engine is reached
 * only through vdi_outline.c.
 */

#include "emutos.h"
#include "string.h"
#include "cookie.h"
#include "vdi_defs.h"
#include "lineavars.h"
#include "biosext.h"
#include "vdi_ft2.h"
#include "vdi_ftos.h"
#include "vdi_kept.h"

#if CONF_WITH_VDI_OUTLINE_FONTS

#define GDOS_FSM            0x5F46534DL /* '_FSM': SpeedoGDOS, or NVDI with vector fonts */

#define NVDI_VERSION        0x0501
#define NVDI_DATE           0x13052005L /* the date fVDI gives, 13 May 2005 */
#define NVDI_FLAG_GDOS      0x0001
#define NVDI_COOKIE_SIZE    184         /* holds every offset read of NVDI 5's data */
#define NVDI_SCAN_FONTS     0xAC        /* NVDI 5's font vectors, from the cookie */
#define NVDI_GET_FONT_DIR   0xB0

#define FORMAT_BITMAP       1           /* vqt_xfntinfo() font formats */
#define FORMAT_TRUETYPE     4

#define MAX_POINTS          32000L

typedef struct {
    UWORD version;              /* BCD */
    ULONG date;                 /* BCD, day month year */
    UWORD flags;
    UBYTE reserved[NVDI_COOKIE_SIZE - 8];   /* NVDI's data: zero but for the two vectors */
} NvdiCookie;

static NvdiCookie *own_cookie;

/* in vdi_asm.S: the vectors, with NVDI's register interface */
void nvdi_scan_fonts_entry(void);
void nvdi_get_font_dir_entry(void);

/* called from vdi_asm.S */
LONG nvdi_scan_fonts(LONG count, LONG flags, char **dirs);
LONG nvdi_get_font_dir(LONG index, char *buf);
LONG vdi_trap_gdos_version(void);


/*
 * ===========================================================================
 * The GDOS answer and the NVDI cookie
 * ===========================================================================
 */

/*
 * vq_vgdos(): what trap #2 with d0 = -2 answers (vdi_asm.S), and what any
 * other trap #2 front end of this VDI should answer
 *
 * '_FSM', SpeedoGDOS's answer, which NVDI-aware programs take as a vector
 * font GDOS.  -2, no GDOS, once the fonts folder is known to hold no font.
 */
LONG vdi_gdos_version(void)
{
    return outline_fonts_present() ? GDOS_FSM : -2L;
}


/*
 * vq_vgdos() through this VDI's own trap #2 (vdi_asm.S)
 *
 * A process the MiNT kernel started itself is told there is no GDOS, as
 * before outline fonts: XaAES asks during its start, from its AES thread,
 * and keeps to the system font then.  Under MiNT the VDI is called from
 * inside the kernel, so the caller's mode does not tell the system from a
 * program; the OS glue tells (vdi_caller_is_system()).  That thread could
 * not scan the fonts folder anyway (vdi_outline.c).
 */
LONG vdi_trap_gdos_version(void)
{
    if (vdi_caller_is_system())
        return -2L;

    return vdi_gdos_version();
}


/* the NVDI cookie, if no VDI has added one yet; called by each workstation opened */
void nvdi_add_cookie(void)
{
    NvdiCookie *c = own_cookie;
    ULONG value;

    if (cookie_get(COOKIE_NVDI, &value))
        return;

    /*
     * Programs read it in user mode, so it lives in a block that is kept
     * and that every program can read, as fVDI's (Mxalloc mode 0x4043)
     */
    if (!c)
    {
        c = vdi_alloc_readable(sizeof(NvdiCookie));
        if (!c)
            return;
        bzero(c, sizeof(NvdiCookie));
        c->version = NVDI_VERSION;
        c->date = NVDI_DATE;
        c->flags = NVDI_FLAG_GDOS;
        ULONG_AT((UBYTE *)c + NVDI_SCAN_FONTS) = (ULONG)nvdi_scan_fonts_entry;
        ULONG_AT((UBYTE *)c + NVDI_GET_FONT_DIR) = (ULONG)nvdi_get_font_dir_entry;
        own_cookie = c;
    }

    cookie_add(COOKIE_NVDI, (ULONG)c);
}


/* TRUE if this is the value of the NVDI cookie that this VDI has added */
BOOL vdi_own_nvdi_cookie(ULONG value)
{
    return own_cookie && (value == (ULONG)own_cookie);
}


/*
 * NVDI 5's scan_fonts: look in these folders for fonts that are not there
 * yet, and add them after the others; returns how many were added
 *
 * NVDI builds its font list anew.  As fVDI, this only adds: a font a
 * program holds the id of is never taken away.  flags are not used.
 */
LONG nvdi_scan_fonts(LONG count, LONG flags, char **dirs)
{
    UNUSED(flags);

    return outline_rescan(dirs, count);
}


/* NVDI 5's get_font_dir: font folder number index, from 1, with a trailing backslash */
LONG nvdi_get_font_dir(LONG index, char *buf)
{
    if (!buf || (index != 1))
        return 0;

    strcpy(buf, outline_font_dir());
    strcat(buf, "\\");

    return 1;
}


/*
 * ===========================================================================
 * The text calls
 * ===========================================================================
 */

static WORD clamp_word(LONG value)
{
    if (value > 32767L)
        return 32767;
    if (value < -32768L)
        return -32768;

    return (WORD)value;
}


/*
 * vqt_char_index() (190): a character's code in another character set,
 * 0xffff if there is none (or if the font has no such character)
 *
 * The bitmap fonts know only the Atari set.
 */
void vdi_vqt_char_index(Vwk * vwk)
{
    UWORD ch = INTIN[0];
    WORD from = INTIN[1], to = INTIN[2];

    if (outline_font_selected(vwk))
        INTOUT[0] = outline_char_index(vwk, ch, from, to);
    else
        INTOUT[0] = ((from == VDI_FT_MAP_ATARI) && (to == VDI_FT_MAP_ATARI) && (ch < 256)) ? ch : -1;
}


/* the sizes of a bitmap face, in points, into sizes[max] */
static WORD bitmap_sizes(WORD id, WORD *sizes, WORD max)
{
    const Fonthead **chain_ptr = font_ring, *f;
    WORD n = 0, i;

    while ((f = *chain_ptr++)) {
        do {
            if (f->font_id == id) {
                for (i = 0; (i < n) && (sizes[i] != f->point); i++)
                    ;
                if ((i == n) && (n < max))
                    sizes[n++] = f->point;
            }
        } while ((f = f->next_font));
    }

    return n;
}


/*
 * vqt_xfntinfo() for a font the engine does not describe: a bitmap font,
 * or an outline face while the engine cannot be called (fVDI's answer for
 * its bitmap fonts)
 */
static void bitmap_xfntinfo(const Fonthead *font, WORD flags, VdiXfntInfo *info)
{
    WORD i;

    if (flags & VDI_XFNT_FONT_NAME) {
        for (i = 0; (i < FONT_NAME_LEN) && font->name[i]; i++)
            info->font_name[i] = font->name[i];
        info->font_name[i] = '\0';
    }
    if (flags & VDI_XFNT_FAMILY_NAME)
        info->family_name[0] = '\0';
    if (flags & VDI_XFNT_STYLE_NAME)
        info->style_name[0] = '\0';
    if (flags & VDI_XFNT_FILE_NAME1)
        info->file_name1[0] = '\0';
    if (flags & VDI_XFNT_FILE_NAME2)
        info->file_name2[0] = '\0';
    if (flags & VDI_XFNT_FILE_NAME3)
        info->file_name3[0] = '\0';
    if (flags & (VDI_XFNT_SIZES | VDI_XFNT_SIZES2))
        info->pt_cnt = bitmap_sizes(font->font_id, info->pt_sizes, ARRAY_SIZE(info->pt_sizes));

    /*
     * only asked for by programs that know the larger structure; "looked,
     * and cannot tell" is a different answer from no answer
     */
    if (flags & VDI_XFNT_CLASS) {
        info->font_class = VDI_FCLASS_UNKNOWN;
        info->font_flags = VDI_FFLAG_VALID;
        if (font->flags & F_MONOSPACE)
            info->font_flags |= VDI_FFLAG_MONOSPACED;
        info->family_class = 0;
        bzero(info->panose, sizeof(info->panose));
    }
}


/*
 * vqt_xfntinfo() (229): what is known of a font, the one numbered
 * INTIN[2] (as vqt_name() numbers them) or else the one with id INTIN[1],
 * or else the current font; INTOUT[0-2] are its format, id and number, all
 * 0 if there is no such font
 */
void vdi_vqt_xfntinfo(Vwk * vwk)
{
    WORD flags = INTIN[0], id = INTIN[1], number = INTIN[2];
    VdiXfntInfo *info = (VdiXfntInfo *)ULONG_AT(&INTIN[3]);
    const Fonthead *font = NULL;
    VdiOutlineFont *face;

    outline_scan();
    if (number)
        font = text_font_number(vwk, number);
    else
        number = text_font_index(vwk, id ? id : vwk->cur_font->font_id, &font);

    INTOUT[0] = INTOUT[1] = INTOUT[2] = 0;
    if (!info)
        return;
    if (!font) {
        info->format = 0;
        info->id = 0;
        info->index = 0;
        return;
    }

    /*
     * fVDI's class fields follow the structure: they are only given to a
     * caller whose structure has them (XaAES asks for every field)
     */
    if (info->size < (LONG)sizeof(VdiXfntInfo))
        flags &= ~VDI_XFNT_CLASS;

    face = outline_face_of(font);
    info->format = face ? FORMAT_TRUETYPE : FORMAT_BITMAP;
    info->id = font->font_id;
    info->index = number;
    if (!face || !outline_xfntinfo(face, flags, info))
        bitmap_xfntinfo(font, flags, info);

    INTOUT[0] = info->format;
    INTOUT[1] = info->id;
    INTOUT[2] = info->index;
}


/*
 * a name wanted, NUL-terminated, against a font name of at most width
 * characters that ends in a NUL or in blanks (what follows a NUL does not
 * count); case does not matter
 */
static BOOL name_matches(const char *wanted, const char *name, WORD width)
{
    WORD i;
    char a, b;

    for (i = 0; i < width; i++) {
        a = wanted[i];
        b = name[i];
        if ((a >= 'A') && (a <= 'Z'))
            a += 'a' - 'A';
        if ((b >= 'A') && (b <= 'Z'))
            b += 'a' - 'A';

        if (a == '\0') {
            while ((i < width) && (name[i] == ' '))
                i++;
            return (i >= width) || (name[i] == '\0');
        }
        if (a != b)
            return FALSE;
    }

    return wanted[width] == '\0';
}


/*
 * vqt_name_and_id() (230 sub 100): the id of the font named in INTIN[1-],
 * one character a word, and the name matched in INTOUT[1-32]; 0 if no font
 * has that name.  INTIN[0], which of a font's names this is, is not used:
 * fonts have only the one.
 */
void vdi_vqt_name_and_id(Vwk * vwk)
{
    char wanted[64];
    WORD count = CONTRL[3] - 1, i, n;
    const Fonthead *f;

    if (count < 0)
        count = 0;
    if (count > (WORD)sizeof(wanted) - 1)
        count = sizeof(wanted) - 1;
    for (i = 0; i < count; i++)
        wanted[i] = (char)INTIN[1+i];
    wanted[count] = '\0';

    /* names travel in fields of fixed width: trailing blanks are normal */
    while ((count > 0) && ((wanted[count-1] == ' ') || (wanted[count-1] == '\t')))
        wanted[--count] = '\0';

    outline_scan();

    for (n = 1; (f = text_font_number(vwk, n)) != NULL; n++)
        if (name_matches(wanted, f->name, FONT_NAME_LEN))
            break;

    INTOUT[0] = 0;
    i = 0;
    if (f) {
        INTOUT[0] = f->font_id;
        for ( ; (i < FONT_NAME_LEN) && f->name[i]; i++)
            INTOUT[1+i] = (UBYTE)f->name[i];
    }
    for ( ; i < FONT_NAME_LEN; i++)
        INTOUT[1+i] = 0;
}


/*
 * vst_width() (231): the width of the widest character of an outline font
 * in pixels, PTSIN[0], with its height unchanged (as NVDI); a bitmap font
 * keeps its width.  PTSOUT gets the sizes, as from vst_point().
 */
void vdi_vst_width(Vwk * vwk)
{
    if (outline_font_selected(vwk))
        outline_set_width(vwk, PTSIN[0]);

    text_size_out(vwk);
}


/*
 * vqt_fontheader() (232): the header of the current font into the buffer
 * at INTIN[0-1]; the file name is not given
 */
void vdi_vqt_fontheader(Vwk * vwk)
{
    VdiFontHeader *fhdr = (VdiFontHeader *)ULONG_AT(&INTIN[0]);
    const Fonthead *font = vwk->cur_font;
    WORD i;

    if (!fhdr)
        return;
    if (outline_face_selected(vwk) && outline_fontheader(vwk, fhdr))
        return;

    /* what fVDI says of its bitmap fonts */
    bzero(fhdr, sizeof(VdiFontHeader));
    memcpy(fhdr->fh_fmver, "D1.0\r\n\0\0", 8);
    fhdr->fh_hedsz = sizeof(VdiFontHeader);
    for (i = 0; (i < FONT_NAME_LEN) && font->name[i]; i++)
        fhdr->fh_fntnm[i] = font->name[i];
    fhdr->fh_cflgs = (font->flags & F_MONOSPACE) ? 0x02 : 0x00;
    fhdr->fh_frmcl = 0x68;      /* normal width and weight */
    fhdr->fh_orupm = 2048;
}


/* vqt_trackkern() (234), vqt_pairkern() (235): there is no kerning */
void vdi_vqt_kerns(Vwk * vwk)
{
    UNUSED(vwk);

    INTOUT[0] = INTOUT[1] = INTOUT[2] = INTOUT[3] = 0;
}


/*
 * vst_charmap(), vst_map_mode() (236): the character set of the text,
 * INTIN[0] (VDI_FT_MAP_*); INTOUT[0] is the one set, the Atari set unless
 * an outline font is selected
 */
void vdi_vst_charmap(Vwk * vwk)
{
    INTOUT[0] = outline_set_map(vwk, INTIN[0]);
}


/* vst_kern(), vst_track_offset() (237): no kerning is done, none reported */
void vdi_vst_kern(Vwk * vwk)
{
    UNUSED(vwk);

    INTOUT[0] = INTOUT[1] = 0;
}


/*
 * v_getoutline() (243) and v_get_outline() (243 sub 1): the outline of
 * character INTIN[0] of the outline font in use, as v_bez points into the
 * arrays at INTIN[2-3] and INTIN[4-5], at most INTIN[1] of them.  INTOUT[0]
 * is their number, 0 if there is no outline or it does not fit.
 *
 * v_getoutline() puts y 0 at the top of the character cell, as NVDI does.
 * v_get_outline() places the character as v_gtext() would at INTIN[6],
 * INTIN[7].  The text effects and rotation are not applied (as fVDI).
 */
void vdi_v_getoutline(Vwk * vwk)
{
    const Fonthead *font = vwk->cur_font;
    WORD sub = CONTRL[5], max = INTIN[1], n, i, advance;
    WORD *xy;
    BYTE *flags;
    LONG x0, base;

    INTOUT[0] = 0;
    if (!outline_font_selected(vwk) || (CONTRL[3] < ((sub == 1) ? 8 : 6)) || (max <= 0))
        return;
    xy = (WORD *)ULONG_AT(&INTIN[2]);
    flags = (BYTE *)ULONG_AT(&INTIN[4]);
    if (!xy || !flags)
        return;

    n = outline_get_outline(vwk, INTIN[0], xy, flags, max);
    if (n <= 0)
        return;

    if (sub == 1) {
        x0 = INTIN[6];
        if (vwk->h_align && outline_char_width(vwk, INTIN[0], &advance))
            x0 -= (vwk->h_align == 1) ? advance / 2 : advance;

        base = INTIN[7];
        switch(vwk->v_align) {
        case 1:
            base += font->half;
            break;
        case 2:
            base += font->ascent;
            break;
        case 3:
            base -= font->bottom;
            break;
        case 4:
            base -= font->descent;
            break;
        case 5:
            base += font->top;
            break;
        }
    } else {
        x0 = 0;
        base = font->top;
    }

    for (i = 0; i < n; i++) {
        xy[2*i] = clamp_word(x0 + xy[2*i]);
        xy[2*i+1] = clamp_word(base - xy[2*i+1]);
    }
    INTOUT[0] = n;
}


/*
 * vst_arbpt() (246) and vst_arbpt32() (246 with two INTIN words, the size
 * in 1/65536 points): any size of an outline font, not only those of its
 * list; INTOUT the size set, in the same unit, and PTSOUT the sizes as
 * from vst_point().  A bitmap font is sized as by vst_point().
 */
void vdi_vst_arbpt(Vwk * vwk)
{
    BOOL wide = (CONTRL[3] >= 2), sized;
    LONG point;
    WORD *old_intin, pt;

    if (wide) {
        point = (LONG)ULONG_AT(&INTIN[0]);
        if (point < 0)
            point = 0;
        if (point > (MAX_POINTS << 16))
            point = MAX_POINTS << 16;
        point = (point + 0x8000L) >> 16;
    } else
        point = INTIN[0];

    sized = FALSE;
    if (outline_face_selected(vwk)) {
        vwk->pts_mode = TRUE;
        sized = outline_set_arbpt(vwk, point);
        if (sized) {
            text_size_out(vwk);
            pt = vwk->cur_font->point;
        } else
            vwk->cur_font = &fon6x6;    /* the face cannot be used: the system font */
    }

    if (!sized) {
        pt = (point > 0) ? (WORD)point : 1;
        old_intin = INTIN;
        INTIN = &pt;
        vdi_vst_point(vwk);
        INTIN = old_intin;
        pt = INTOUT[0];
    }

    if (wide) {
        ULONG_AT(&INTOUT[0]) = (ULONG)((LONG)pt << 16);
        CONTRL[4] = 2;
    } else
        INTOUT[0] = pt;
}


/* the advance of a character, as vqt_width() gives it; 0 if there is none */
static WORD width_advance(Vwk *vwk, WORD ch)
{
    WORD *old_intin = INTIN, *old_intout = INTOUT, *old_ptsout = PTSOUT;
    WORD in = ch, out = -1, pts[6];

    pts[0] = 0;
    INTIN = &in;
    INTOUT = &out;
    PTSOUT = pts;
    vdi_vqt_width(vwk);
    INTIN = old_intin;
    INTOUT = old_intout;
    PTSOUT = old_ptsout;

    return (out == ch) ? pts[0] : 0;
}


/*
 * vqt_advance() and vqt_advance32() (247): the advance of character
 * INTIN[0] in pixels, PTSOUT[0] and [1] across and down, the remainders in
 * PTSOUT[2-3], and the same in 1/65536 pixels in PTSOUT[4-7]
 *
 * An outline font takes any code of its character set; text is never
 * drawn down, so there are no remainders and no advance down.
 */
void vdi_vqt_advance(Vwk * vwk)
{
    WORD advance = 0;

    if (!outline_font_selected(vwk) || !outline_char_width(vwk, INTIN[0], &advance))
        advance = width_advance(vwk, INTIN[0]);

    PTSOUT[0] = advance;
    PTSOUT[1] = PTSOUT[2] = PTSOUT[3] = 0;
    ULONG_AT(&PTSOUT[4]) = (ULONG)((LONG)advance << 16);
    ULONG_AT(&PTSOUT[6]) = 0;
    flip_y = 1;
}


/*
 * vst_setsize() (252) and vst_setsize32() (252 with two INTIN words, in
 * 1/65536 points): an outline font made as wide as that size would be,
 * with its height unchanged; INTOUT is the width set, in the same unit, 0
 * if none was (a bitmap font).  PTSOUT gets the sizes, as from vst_point().
 */
void vdi_vst_setsize(Vwk * vwk)
{
    BOOL wide = (CONTRL[3] >= 2);
    LONG width, result = 0;

    if (wide)
        width = (LONG)ULONG_AT(&INTIN[0]);
    else
        width = (INTIN[0] > 0) ? ((LONG)INTIN[0] << 16) : 0;

    if (outline_font_selected(vwk) && (width > 0) && (width <= (MAX_POINTS << 16)) &&
        outline_set_size(vwk, width))
        result = width;

    text_size_out(vwk);

    if (wide) {
        ULONG_AT(&INTOUT[0]) = (ULONG)result;
        CONTRL[4] = 2;
    } else
        INTOUT[0] = (WORD)(result >> 16);
}


/*
 * vst_skew() (253): the slant of outline text in tenths of a degree,
 * positive to the right, INTIN[0]; INTOUT[0] is the slant set
 */
void vdi_vst_skew(Vwk * vwk)
{
    INTOUT[0] = outline_set_skew(vwk, INTIN[0]);
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS */
