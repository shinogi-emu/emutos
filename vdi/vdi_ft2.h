/*
 * vdi_ft2.h - the boundary between the VDI and the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The outline font engine (vdi_ft2.c) and FreeType are compiled with
 * 32-bit int, while the rest of EmuTOS is compiled with -mshort.  The
 * two halves meet only through the functions below, which take and
 * return nothing but LONG, ULONG and pointers: those are passed the
 * same way by both ABIs.
 *
 * Therefore this header must not use int, enum or bit-fields, and every
 * function argument must be a LONG, ULONG or a pointer.
 */

#ifndef VDI_FT2_H
#define VDI_FT2_H

#include "portab.h"

#ifndef VDI_FT_BYTE_DEFINED
#define VDI_FT_BYTE_DEFINED
typedef signed char BYTE;
#endif

/*
 * The engine and FreeType need a stack of VDI_FT_STACK_SIZE bytes.
 * FreeType bounds neither the nesting of TrueType composite glyphs nor
 * that of CFF glyphs built of others (seac), but it allocates memory at
 * every level: the allocator (vdi_ftmem.c, which every vdi_ft_alloc() and
 * vdi_ft_realloc() goes through) refuses a block when fewer than
 * VDI_FT_STACK_GUARD bytes of that stack are left, so that a glyph nested
 * too deep fails as one would for want of memory, instead of overflowing
 * the stack.
 */
#define VDI_FT_STACK_SIZE   65536L
#define VDI_FT_STACK_GUARD  20480L

/* values for VdiOutlineFont.flags */
#define VDI_FT_MONOSPACE    0x00000001L
#define VDI_FT_SCALABLE     0x00000002L

/*
 * values for the map arguments: the character set the codes of a string
 * are given in (vst_charmap and vst_map_mode)
 */
#define VDI_FT_MAP_BICS     0L      /* Bitstream International Character Set */
#define VDI_FT_MAP_ATARI    1L      /* the Atari character set (default) */
#define VDI_FT_MAP_UNICODE  2L

typedef struct {            /* one face at one size, owned by vdi_ft2.c */
    LONG id;                /* VDI font id (fVDI's ft2_get_face_id rule) */
    LONG points;            /* size in points */
    LONG pixel_height;      /* top + bottom */
    LONG top, ascent, half, descent, bottom;
    LONG max_char_width, max_cell_width;
    LONG underline_size, underline_offset;
    LONG first_ade, last_ade;
    LONG flags;             /* VDI_FT_MONOSPACE, VDI_FT_SCALABLE */
    char name[32];          /* VDI name, Atari code page, NUL-terminated */
    void *private;          /* FT_Face, cache etc.; opaque to -mshort code */
} VdiOutlineFont;

typedef struct {            /* a rendered string or glyph */
    UWORD *data;            /* 1 plane, rows padded to 16 pixels */
    LONG width, height;     /* pixels */
    LONG wdwidth;           /* words per row */
    LONG origin_x, origin_y;/* baseline origin inside the bitmap */
    LONG advance_x;         /* pen advance, pixels */
} VdiOutlineBitmap;

/*
 * what vdi_ft_xfntinfo() fills in: the XFNT_INFO structure of
 * vqt_xfntinfo(), followed by fVDI's classification fields.  size,
 * format, id and index are left to the caller; outline fonts have
 * format 4 (TrueType), the CFF ones too.
 */
typedef struct {
    LONG size;
    WORD format;
    WORD id;
    WORD index;
    char font_name[50];
    char family_name[50];
    char style_name[50];
    char file_name1[200];
    char file_name2[200];
    char file_name3[200];
    WORD pt_cnt;
    WORD pt_sizes[64];
    /*
     * only filled in when VDI_XFNT_CLASS is asked for: a caller that does
     * not know these fields never sets that bit, so nothing is written
     * past the end of its smaller structure
     */
    WORD font_class;        /* VDI_FCLASS_* */
    UWORD font_flags;       /* VDI_FFLAG_* */
    WORD family_class;      /* OS/2 sFamilyClass, 0 if none */
    UBYTE panose[10];       /* OS/2 PANOSE, zeroes if none */
} VdiXfntInfo;

/* flags for vdi_ft_xfntinfo(): the fields to fill in */
#define VDI_XFNT_FONT_NAME      0x0001L
#define VDI_XFNT_FAMILY_NAME    0x0002L
#define VDI_XFNT_STYLE_NAME     0x0004L
#define VDI_XFNT_FILE_NAME1     0x0008L
#define VDI_XFNT_FILE_NAME2     0x0010L
#define VDI_XFNT_FILE_NAME3     0x0020L
#define VDI_XFNT_SIZES          0x0100L
#define VDI_XFNT_SIZES2         0x0200L
#define VDI_XFNT_CLASS          0x0400L

/* values for VdiXfntInfo.font_class */
#define VDI_FCLASS_UNKNOWN      0
#define VDI_FCLASS_SERIF        1
#define VDI_FCLASS_SANS         2
#define VDI_FCLASS_SCRIPT       3
#define VDI_FCLASS_DECORATIVE   4
#define VDI_FCLASS_SYMBOL       5

/* bits in VdiXfntInfo.font_flags */
#define VDI_FFLAG_MONOSPACED    0x0001
#define VDI_FFLAG_VALID         0x8000      /* the class fields were filled in */

/* what vdi_ft_fontheader() fills in: the header of vqt_fontheader() */
typedef struct {
    char fh_fmver[8];       /* format identifier */
    ULONG fh_fntsz;         /* font file size */
    ULONG fh_fbfsz;         /* minimum font buffer size */
    UWORD fh_cbfsz;         /* minimum character buffer size */
    UWORD fh_hedsz;         /* header size */
    UWORD fh_fntid;         /* font id (Bitstream) */
    UWORD fh_sfvnr;         /* font version number */
    char fh_fntnm[70];      /* full font name (vqt_name) */
    char fh_mdate[10];      /* manufacturing date */
    char fh_laynm[70];      /* character set name, vendor and set id */
    char fh_cpyrt[78];      /* copyright notice */
    UWORD fh_nchrl;         /* character indices in the character set */
    UWORD fh_nchrf;         /* character indices in the font */
    UWORD fh_fchrf;         /* index of the first character */
    UWORD fh_nktks;         /* kerning tracks */
    UWORD fh_nkprs;         /* kerning pairs */
    char fh_flags;          /* bit 0: extended mode */
    char fh_cflgs;          /* bit 0: italic, 1: monospace */
    char fh_famcl;          /* family classification */
    char fh_frmcl;          /* form classification (weight and width) */
    char fh_sfntn[32];      /* PostScript name */
    char fh_sfacn[16];      /* family name */
    char fh_fntfm[14];      /* style name */
    UWORD fh_itang;         /* italic angle */
    UWORD fh_orupm;         /* outline resolution units per em */
} VdiFontHeader;

/*
 * The functions below take the sized fonts returned by vdi_ft_size();
 * given an unsized face, they use it at 10 points, 10 pixels.
 *
 * vdi_ft_size(): pixel_height is the size of the em in pixels, that is
 * points converted with the vertical resolution of the device.
 * char_width sets the width of the widest character independently of the
 * height (vst_width), 0 keeps the natural width.  A sized font stays
 * valid until vdi_ft_term().
 *
 * effects are the VDI text effects; skew is in tenths of a degree,
 * positive leaning right (italic without skew leans 12 degrees).
 * Codes are taken as unsigned 16-bit numbers in the character set given
 * by map; characters the font lacks are drawn as its .notdef glyph.
 *
 * vdi_ft_render() returns 0 and a bitmap to give back to vdi_ft_release()
 * (data is NULL if there is nothing to draw), or a negative number if
 * the text could not be drawn.  Bit 0x80 of the first byte of a row is
 * its leftmost pixel.  It draws the underline (effect 8) itself; leave
 * that bit out to draw it from underline_size and underline_offset.
 * vdi_ft_text_extent() measures without skew; a monospaced font is as
 * wide as its advances add up to.
 *
 * vdi_ft_char_advance() gives the horizontal advance in advance[0]; the
 * vertical advance and both remainders are 0.
 *
 * vdi_ft_get_outline() gives v_bez points in pixels, y up from the
 * baseline, and returns their number, 0 if there is no outline, or -1 if
 * there are more than max_points.
 */
LONG vdi_ft_init(void);                                   /* 0 ok, <0 error */
void vdi_ft_term(void);
LONG vdi_ft_register_file(const char *path);              /* faces added, <0 bad file */
LONG vdi_ft_face_count(void);
VdiOutlineFont *vdi_ft_face(LONG index);                  /* registered face, unsized */
VdiOutlineFont *vdi_ft_size(VdiOutlineFont *face, LONG points, LONG pixel_height,
                            LONG char_width);
LONG vdi_ft_text_extent(VdiOutlineFont *f, const WORD *text, LONG count,
                        LONG map, LONG effects, LONG *width, LONG *height);
LONG vdi_ft_char_advance(VdiOutlineFont *f, LONG ch, LONG map, LONG effects,
                         LONG *advance /* [4] */);
LONG vdi_ft_render(VdiOutlineFont *f, const WORD *text, LONG count,
                   LONG map, LONG effects, LONG skew, VdiOutlineBitmap *out);
void vdi_ft_release(VdiOutlineBitmap *bitmap);
LONG vdi_ft_char_index(VdiOutlineFont *f, LONG ch, LONG from_mode, LONG to_mode);
LONG vdi_ft_get_outline(VdiOutlineFont *f, LONG ch, LONG map, WORD *xy, BYTE *flags,
                        LONG max_points);
void vdi_ft_xfntinfo(VdiOutlineFont *f, LONG flags, void *xfnt_info);
void vdi_ft_fontheader(VdiOutlineFont *f, void *vqt_fhdr);

#endif /* VDI_FT2_H */
