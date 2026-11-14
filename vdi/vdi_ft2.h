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

/* values for VdiOutlineFont.flags */
#define VDI_FT_MONOSPACE    0x00000001L
#define VDI_FT_SCALABLE     0x00000002L

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

LONG vdi_ft_init(void);                                   /* 0 ok, <0 error */
void vdi_ft_term(void);
LONG vdi_ft_register_file(const char *path);              /* faces added, <0 bad file */
LONG vdi_ft_face_count(void);
VdiOutlineFont *vdi_ft_face(LONG index);                  /* registered face, unsized */
VdiOutlineFont *vdi_ft_size(VdiOutlineFont *face, LONG points, LONG pixel_height);
LONG vdi_ft_text_extent(VdiOutlineFont *f, const WORD *text, LONG count,
                        LONG effects, LONG *width, LONG *height);
LONG vdi_ft_char_advance(VdiOutlineFont *f, LONG ch, LONG *advance /* [4] */);
LONG vdi_ft_render(VdiOutlineFont *f, const WORD *text, LONG count,
                   LONG effects, LONG skew, VdiOutlineBitmap *out);
void vdi_ft_release(VdiOutlineBitmap *bitmap);
LONG vdi_ft_char_index(VdiOutlineFont *f, LONG ch, LONG from_mode, LONG to_mode);
LONG vdi_ft_get_outline(VdiOutlineFont *f, LONG ch, WORD *xy, BYTE *flags,
                        LONG max_points);
void vdi_ft_xfntinfo(VdiOutlineFont *f, LONG flags, void *xfnt_info);
void vdi_ft_fontheader(VdiOutlineFont *f, void *vqt_fhdr);

#endif /* VDI_FT2_H */
