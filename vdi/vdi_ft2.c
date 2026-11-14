/*
 * vdi_ft2.c - the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * It is fVDI's FreeType module (modules/ft2/ft2.c), which carries this
 * notice:
 *
 * fVDI font load and setup
 *
 * Copyright 1997-2000/2003, Johan Klockars
 *                     2005, Standa Opichal
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

/*
 * This file is compiled with 32-bit int, like FreeType (see the Makefile).
 * It only meets the rest of the VDI through vdi_ft2.h, and uses the file
 * and memory services of vdi_ftos.h.  It knows nothing of workstations:
 * a face can be sized and drawn before any workstation exists.
 *
 * The objects:
 *
 * - a registered face (FaceRec) is a font file, or one face of it, with
 *   what vqt_name, vqt_xfntinfo and vqt_fontheader report about it;
 * - a size (SizeRec) is a face at one size and width, with its metrics.
 *   Sizes are what the VDI selects and draws with, and they stay valid
 *   until vdi_ft_term();
 * - an instance (fVDI's per-size Fontheader) is a size with one set of
 *   effects and skew: an open FreeType face and the glyphs it has
 *   rendered.  Instances are the expensive part, so only the most
 *   recently used are kept, and a size whose instances were dropped opens
 *   one again when it is next used;
 * - a font file (FontFile) is a whole file read into memory, shared by
 *   the instances of its faces.
 */

#include "vdi_ft2.h"
#include "vdi_ftos.h"
#include "vdi_ftmap.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H
#include FT_STROKER_H
#include FT_OUTLINE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRIGONOMETRY_H
#include FT_MODULE_H


/* Handy routines for converting from fixed point */
#define FT_FLOOR(X)     (((X) & -64) / 64)
#define FT_CEIL(X)      ((((X) + 63) & -64) / 64)

/* what a cached glyph holds */
#define CACHED_METRICS  0x10
#define CACHED_BITMAP   0x01
#define CACHED_INDEX    0x100   /* the glyph index has been looked up */

/* the VDI text effects */
#define EFFECT_BOLD         0x01
#define EFFECT_LIGHT        0x02
#define EFFECT_ITALIC       0x04
#define EFFECT_UNDERLINE    0x08
#define EFFECT_OUTLINE      0x10

/* Underline doesn't warrant a new font cache */
#define FT2_EFFECTS_MASK    0x17

/* the mask light text is drawn through, alternating with its inverse */
#define LIGHTENING          0xaa

/* x offset = cos(((90.0-12)/360)*2*M_PI), or 12 degree angle: 0.207 */
#define ITALIC_SHEAR        13565L      /* 0.207 in 16.16 */

/* skew, in tenths of a degree, is limited to this either way */
#define SKEW_MAX            800L

#define MAX_SIZE            32000L      /* points, pixels and widths */
#define MAX_BITMAP          32767L      /* width or height of a string */
#define DEFAULT_POINTS      10L         /* the size of an unsized face */
#define INSTANCE_MAX        10          /* open instances kept, at least */
#define FF_KEEP_MAX         8           /* unreferenced buffers kept for reuse */
#define MAX_FILE_FACES      32          /* faces registered from one file */
#define GLYPH_TRIES         16          /* glyphs looked at to accept a face */

#define KIND_FACE           0x46616365L /* 'Face' */
#define KIND_SIZE           0x53697a65L /* 'Size' */


/* Cached glyph information */
typedef struct
{
    UWORD stored;           /* CACHED_* */
    UWORD code;             /* the character code ... */
    LONG map;               /* ... in this character set */
    FT_UInt index;          /* the glyph */
    LONG minx, maxx;        /* horizontal extent from the pen, pixels */
    LONG yoffset;           /* top of the glyph below the top of the cell */
    LONG advance;
    LONG left, top;         /* bitmap position from the pen and baseline */
    LONG width, rows, pitch;
    UBYTE *buffer;          /* 1 plane, rows of whole words */
} CachedGlyph;

/* the start of a registered face and of a size */
typedef struct
{
    VdiOutlineFont font;    /* what the caller sees; private points here */
    LONG kind;              /* KIND_FACE or KIND_SIZE */
} FontBase;

typedef struct font_file
{
    struct font_file *next;
    void *data;
    ULONG size;
    LONG refs;
    ULONG stamp;
    char name[1];           /* the file name, allocated with the record */
} FontFile;

struct size_rec;

typedef struct instance
{
    struct instance *newer, *older; /* in the list of all instances */
    struct instance *next;          /* among the instances of its size */
    struct size_rec *size;
    LONG effects;
    LONG skew;
    FT_Face face;
    FontFile *file;
    CachedGlyph cache[256];         /* codes 0-255 */
    CachedGlyph scratch;            /* the last code above 255 */
} Instance;

typedef struct face_rec
{
    FontBase base;
    struct face_rec *next;
    struct size_rec *sizes;
    LONG index;                     /* of the face in its file */
    /* what vqt_xfntinfo and vqt_fontheader report */
    char family[50];
    char style[50];
    char ps_name[33];
    LONG num_glyphs;
    LONG units_per_em;
    LONG style_flags;
    LONG os2_valid;
    WORD family_class;
    WORD font_class;
    UBYTE panose[10];
    char path[1];                   /* allocated with the record */
} FaceRec;

typedef struct size_rec
{
    FontBase base;
    struct size_rec *next;          /* among the sizes of its face */
    FaceRec *face;
    LONG points;                    /* the size asked for */
    LONG pixels;                    /* size of the em, pixels */
    LONG char_width;                /* vst_width, 0 for the natural width */
    LONG skewing;                   /* bitmap faces: italic slant, pixels */
    Instance *instances;
} SizeRec;


/* the sizes vqt_xfntinfo reports, as fVDI's default size list */
static const WORD pt_sizes[] = { 8, 9, 10, 11, 12, 14, 18, 24, 36, 48 };

/* the FreeType modules this engine needs, as in freetype/config/ftmodule.h */
static const char *const modules[] =
{
    "autofitter", "truetype", "cff", "psaux", "psnames", "pshinter",
    "sfnt", "smooth", "raster1"
};


static FT_Library library;
static FaceRec *faces;              /* registered faces, in order */
static LONG face_count;
static Instance *newest, *oldest;   /* open instances, by last use */
static LONG instance_count;
static FontFile *fontfiles;
static ULONG ff_clock;

/* NVDI/SpeedoGDOS spdchar.map: the first table, Atari codes 32-255 to BICS */
static WORD spdchar_map[224];
static LONG spdchar_map_valid;


static void close_instance(Instance *in);


/*
 * ===========================================================================
 * Font files held in memory
 * ===========================================================================
 *
 * Each font file is read once, in one sequential pass, and handed to
 * FT_New_Memory_Face.  After that a face costs no file operations at all:
 * no handle is held, and GEMDOS handles, which belong to the process that
 * opened them, cannot get mixed up between the applications that draw text.
 *
 * Entries are reference counted, because several instances (sizes and
 * effects of the same font) share one file.  A buffer whose face is alive
 * can never be freed -- FT_New_Memory_Face does not copy -- so the count is
 * what keeps it, not the LRU.
 *
 * Unreferenced buffers are kept for reuse up to FF_KEEP_MAX so that closing
 * and reopening a face does not re-read the file; past that the least
 * recently used are freed.  When a file cannot be read for lack of memory,
 * the unreferenced buffers are freed and it is tried once more.
 */

/* Free the least recently used unreferenced buffers past keep. */
static void ff_trim(LONG keep)
{
    for (;;)
    {
        FontFile **pp, **victim = NULL;
        ULONG oldest_stamp = 0;
        LONG idle = 0;

        for (pp = &fontfiles; *pp; pp = &(*pp)->next)
        {
            if ((*pp)->refs)
                continue;
            idle++;
            if (!victim || (*pp)->stamp < oldest_stamp)
            {
                oldest_stamp = (*pp)->stamp;
                victim = pp;
            }
        }

        if (idle <= keep || !victim)
            return;

        {
            FontFile *f = *victim;

            *victim = f->next;
            vdi_font_file_free(f->data);
            vdi_ft_free(f);
        }
    }
}

static LONG ff_idle(void)
{
    FontFile *f;
    LONG idle = 0;

    for (f = fontfiles; f; f = f->next)
        if (!f->refs)
            idle++;

    return idle;
}

static FontFile *ff_acquire(const char *name)
{
    FontFile *f;
    void *data;
    ULONG size = 0;

    for (f = fontfiles; f; f = f->next)
    {
        if (ft_strcmp(f->name, name) == 0)
        {
            f->refs++;
            f->stamp = ++ff_clock;
            return f;
        }
    }

    f = vdi_ft_alloc(sizeof(FontFile) + ft_strlen(name));
    if (!f && ff_idle())
    {
        ff_trim(0);
        f = vdi_ft_alloc(sizeof(FontFile) + ft_strlen(name));
    }
    if (!f)
        return NULL;

    /* One sequential pass.  This is the only time the file is read. */
    data = vdi_font_file_load(name, &size);
    if (!data && ff_idle())
    {
        ff_trim(0);
        data = vdi_font_file_load(name, &size);
    }
    if (!data)
    {
        vdi_ft_free(f);
        return NULL;
    }

    ft_strcpy(f->name, name);
    f->data = data;
    f->size = size;
    f->refs = 1;
    f->stamp = ++ff_clock;
    f->next = fontfiles;
    fontfiles = f;

    return f;
}

static void ff_release(FontFile *f)
{
    if (f->refs > 0)
        f->refs--;
    if (!f->refs)
    {
        f->stamp = ++ff_clock;
        ff_trim(FF_KEEP_MAX);
    }
}


/*
 * ===========================================================================
 * Character sets
 * ===========================================================================
 */

static UWORD bics_to_unicode(UWORD bics)
{
    return (bics < BICS_COUNT) ? Bics2Unicode[bics] : 0xffff;
}

/* an Atari code (0-255) to BICS, through spdchar.map if there is one */
static UWORD atari_to_bics(UWORD ch)
{
    UWORD cc = 0xffff;

    /* if there is no spdchar.map (or not enough data)
     * -> use built-in mapping */
    if (!spdchar_map_valid)
        return (UWORD)Atari2Bics[ch];

    if (ch > 31)
    {
        /* NVDI really seems to be using the _first_ map '00' in the
         * spdchar.map file no matter what the font format is.
         *
         * The TT table seems to contain a lot of crap for non ASCII
         * chars commonly.
         */
        cc = (UWORD)spdchar_map[ch - 32];
    }

    /* in case we don't have BICS code => fallback to the default mapping */
    if (cc >= BICS_COUNT || ch < 32)
        cc = (UWORD)Atari2Bics[ch];

    return cc;
}

static FT_UInt unicode_glyph(FT_Face face, UWORD cc)
{
    return FT_Get_Char_Index(face, (cc == 0xffff) ? (FT_ULong)-1 : cc);
}

/* the glyph for a character code in a character set */
static FT_UInt glyph_index(FT_Face face, UWORD ch, LONG map)
{
    FT_UInt index;
    UWORD cc;

    if (map == VDI_FT_MAP_ATARI && ch < 256)
    {
        /* in case we don't have BICS -> UNICODE the cc value will be -1 */
        cc = bics_to_unicode(atari_to_bics(ch));
        index = unicode_glyph(face, cc);

        /* When there is no such character in the font (or cc was -1)
         * we fallback to the default built-in mapping */
        if (!index)
        {
            cc = bics_to_unicode((UWORD)Atari2Bics[ch]);

            /* sanity check */
            if (cc == 0xffff)
                cc = ch;

            index = unicode_glyph(face, cc);
        }
        return index;
    }

    if (map == VDI_FT_MAP_UNICODE)
    {
        /* app use: used at least by the 'Highwire web browser' */
        return FT_Get_Char_Index(face, ch);
    }

    if (map == VDI_FT_MAP_BICS)
    {
        /* BICS: no char -> index translation, BICS char index is expected */
        cc = bics_to_unicode(ch);
        if (cc == 0xffff)
            cc = ch;
        return FT_Get_Char_Index(face, cc);
    }

    /* anything else is taken as a glyph index, as fVDI does */
    return ch;
}

/* vqt_char_index: a code from one character set to another, 0xffff if none */
static LONG map_char(FT_Face face, UWORD ch, LONG src_mode, LONG dst_mode)
{
    FT_UInt index;
    LONG i;

    /*
     * same logic here as in glyph_index()
     */
    if (src_mode == VDI_FT_MAP_ATARI && ch < 256)
    {
        UWORD cc = bics_to_unicode(atari_to_bics(ch));

        index = unicode_glyph(face, cc);

        /* When there is no such character in the font (or cc was -1)
         * we fallback to the default built-in mapping */
        if (!index)
        {
            cc = bics_to_unicode((UWORD)Atari2Bics[ch]);
            index = unicode_glyph(face, cc);
        }
        ch = cc;
    }
    else if (src_mode == VDI_FT_MAP_UNICODE)
        index = FT_Get_Char_Index(face, ch);
    else if (src_mode == VDI_FT_MAP_BICS)
    {
        ch = bics_to_unicode(ch);
        index = unicode_glyph(face, ch);
    }
    else
        index = 0;

    if (!index)
        return 0xffff;

    if (dst_mode == VDI_FT_MAP_UNICODE)
        return ch;

    if (dst_mode == VDI_FT_MAP_ATARI)
    {
        for (i = spdchar_map_valid ? 32 : 0; i < 256; i++)
        {
            WORD bics = spdchar_map_valid ? spdchar_map[i - 32] : Atari2Bics[i];

            if (bics >= 0 && bics < BICS_COUNT && Bics2Unicode[bics] == ch)
                return i;
        }
    }
    else if (dst_mode == VDI_FT_MAP_BICS)
    {
        for (i = 0; i < 256; i++)
            if (Bics2Unicode[i] == ch)
                return i;
    }

    return 0xffff;
}


/*
 * ===========================================================================
 * Instances: a size with its effects, an open face and its glyphs
 * ===========================================================================
 */

static void flush_glyph(CachedGlyph *glyph)
{
    vdi_ft_free(glyph->buffer);
    ft_memset(glyph, 0, sizeof(*glyph));
}

static void flush_cache(Instance *in)
{
    int i;

    for (i = 0; i < 256; i++)
        flush_glyph(&in->cache[i]);
    flush_glyph(&in->scratch);
}

static void flush_all_caches(void)
{
    Instance *in;

    for (in = newest; in; in = in->older)
        flush_cache(in);
}

static void lru_unlink(Instance *in)
{
    if (in->newer)
        in->newer->older = in->older;
    else
        newest = in->older;
    if (in->older)
        in->older->newer = in->newer;
    else
        oldest = in->newer;
    in->newer = in->older = NULL;
}

static void lru_insert(Instance *in)
{
    in->older = newest;
    in->newer = NULL;
    if (newest)
        newest->newer = in;
    newest = in;
    if (!oldest)
        oldest = in;
}

/*
 * Set the size of a face and work out the metrics.  The metrics are the
 * same for every instance of a size, so it does not matter which instance
 * stores them.
 */
static int load_metrics(SizeRec *s, FT_Face face)
{
    VdiOutlineFont *font = &s->base.font;
    LONG top, descent, cell, character, ul_offset, ul_size, height;
    FT_Error error;

    if (FT_IS_SCALABLE(face))
    {
        FT_Size_Metrics *m;

        error = FT_Set_Pixel_Sizes(face, 0, s->pixels);
        if (error)
            return -1;
        m = &face->size->metrics;

        top = FT_CEIL(m->ascender);
        descent = FT_CEIL(-m->descender);

        /* This gives us weird values - perhaps caused by taking care of unusual characters out of Latin-1 charset */
        cell = FT_CEIL(FT_MulFix(face->bbox.xMax - face->bbox.xMin, m->x_scale));
        character = FT_CEIL(FT_MulFix(face->max_advance_width, m->x_scale));

        /* Width is a separate transform: changing it must not change the
         * vertical size/hinting. Each transformed face has its own cache. */
        if (s->char_width > 0 && character > 0)
        {
            FT_Matrix matrix;
            LONG c, w;

            matrix.xx = FT_DivFix(s->char_width, character);
            matrix.xy = matrix.yx = 0;
            matrix.yy = 0x10000L;
            c = FT_CEIL(FT_MulFix(cell * 64, matrix.xx));
            w = FT_CEIL(FT_MulFix(character * 64, matrix.xx));
            if (c <= 0 || c > MAX_SIZE || w <= 0 || w > MAX_SIZE)
                return -1;
            FT_Set_Transform(face, &matrix, NULL);
            cell = c;
            character = s->char_width;
        }

        ul_offset = FT_FLOOR(FT_MulFix(face->underline_position, m->y_scale));
        ul_size = FT_FLOOR(FT_MulFix(face->underline_thickness, m->y_scale));
        s->skewing = 0;
        font->points = s->points;
    }
    else if (face->num_fixed_sizes > 0)
    {
        LONG pick = 0, i;

        /* the bitmap size closest to the one asked for */
        for (i = 1; i < face->num_fixed_sizes; i++)
        {
            LONG d = face->available_sizes[i].size / 64 - s->points;
            LONG best = face->available_sizes[pick].size / 64 - s->points;

            if ((d < 0 ? -d : d) < (best < 0 ? -best : best))
                pick = i;
        }

        error = FT_Select_Size(face, pick);
        if (error)
            return -1;

        top = face->available_sizes[pick].height;
        descent = 0;

        /* This gives us weird values - perhaps caused by taking care of unusual characters out of Latin-1 charset */
        cell = face->available_sizes[pick].width;
        character = face->available_sizes[pick].width;

        ul_offset = FT_FLOOR(face->underline_position);
        ul_size = FT_FLOOR(face->underline_thickness);

        /* x offset = cos(((90.0-12)/360)*2*M_PI), or 12 degree angle */
        s->skewing = (207 * top) / 1000 + 1;

        /* Set the closest available bitmap font size */
        font->points = face->available_sizes[pick].size / 64;
    }
    else
        return -1;

    height = top + descent;
    if (height <= 0 || top > MAX_SIZE || descent > MAX_SIZE || height > MAX_SIZE ||
        cell > MAX_SIZE || character > MAX_SIZE)
        return -1;

    font->top = top;
    font->ascent = top;
    font->descent = descent;
    font->bottom = descent;
    font->pixel_height = height;

    /* Font half distance (positive distance from baseline) */
    font->half = top - (height >> 1);

    font->max_cell_width = cell;
    font->max_char_width = character;
    font->underline_offset = ul_offset;
    font->underline_size = (ul_size < 1) ? 1 : ul_size;

    /* Fake for vqt_fontinfo() as some apps might rely on this */
    font->first_ade = 0;
    font->last_ade = 255;

    return 0;
}

static Instance *open_instance(SizeRec *s, LONG effects, LONG skew)
{
    Instance *in;
    FT_Face face;

    /* The least recently used instance makes room.  It belongs to no
     * caller: nothing outlives the call that used it but the size. */
    if (instance_count > INSTANCE_MAX && oldest)
        close_instance(oldest);

    in = vdi_ft_alloc(sizeof(Instance));
    if (!in)
        return NULL;
    ft_memset(in, 0, sizeof(Instance));

    in->file = ff_acquire(s->face->path);
    if (!in->file)
    {
        vdi_ft_free(in);
        return NULL;
    }

    if (FT_New_Memory_Face(library, in->file->data, in->file->size,
                           s->face->index, &face))
    {
        ff_release(in->file);
        vdi_ft_free(in);
        return NULL;
    }

    if (load_metrics(s, face) < 0)
    {
        FT_Done_Face(face);
        ff_release(in->file);
        vdi_ft_free(in);
        return NULL;
    }

    in->face = face;
    in->size = s;
    in->effects = effects;
    in->skew = skew;
    in->next = s->instances;
    s->instances = in;
    lru_insert(in);
    instance_count++;

    return in;
}

static void close_instance(Instance *in)
{
    Instance **pp;

    for (pp = &in->size->instances; *pp; pp = &(*pp)->next)
    {
        if (*pp == in)
        {
            *pp = in->next;
            break;
        }
    }
    lru_unlink(in);
    flush_cache(in);
    FT_Done_Face(in->face);
    ff_release(in->file);
    vdi_ft_free(in);

    /* Only now: the count follows what is really freed. */
    instance_count--;
}

static Instance *get_instance(SizeRec *s, LONG effects, LONG skew)
{
    Instance *in;

    effects &= FT2_EFFECTS_MASK;
    if (skew > SKEW_MAX)
        skew = SKEW_MAX;
    else if (skew < -SKEW_MAX)
        skew = -SKEW_MAX;

    for (in = s->instances; in; in = in->next)
    {
        if (in->effects == effects && in->skew == skew)
        {
            lru_unlink(in);
            lru_insert(in);
            return in;
        }
    }

    return open_instance(s, effects, skew);
}


/*
 * ===========================================================================
 * Glyphs
 * ===========================================================================
 */

/* shift a row of pixels right by count, dropping what falls off its end */
static void shift_row(UBYTE *row, LONG bytes, LONG count)
{
    LONG skip = count >> 3, shift = count & 7, i;

    for (i = bytes - 1; i >= 0; i--)
    {
        LONG from = i - skip;
        UBYTE hi = (from >= 0) ? row[from] : 0;
        UBYTE lo = (from >= 1) ? row[from - 1] : 0;

        row[i] = shift ? (UBYTE)((hi >> shift) | (lo << (8 - shift))) : hi;
    }
}

static FT_Error load_glyph(Instance *in, UWORD ch, LONG map, CachedGlyph *cached, int want)
{
    SizeRec *s = in->size;
    FT_Face face = in->face;
    FT_Error error;
    FT_Glyph g = NULL;
    FT_GlyphSlot glyph;
    LONG bitmap_italics_shear = 0;

    /*
     * Loading a glyph - hinting it - costs several times what rendering it
     * does once it is loaded, so keep the image text will draw it with as
     * well.  A glyph is measured (vqt_extent, or text sizing its buffer)
     * just before it is drawn, and each used to be loaded twice.
     */
    if (want & CACHED_METRICS)
        want |= CACHED_BITMAP;

    /* Load the glyph */
    if (!(cached->stored & CACHED_INDEX))
    {
        cached->index = glyph_index(face, ch, map);
        cached->code = ch;
        cached->map = map;
        cached->stored |= CACHED_INDEX;
    }

    error = FT_Load_Glyph(face, cached->index, FT_LOAD_DEFAULT);
    if (error)
        return error;

    /* Get our glyph shortcuts */
    glyph = face->glyph;

    /* Handle 'Bold' effect */
    if (in->effects & EFFECT_BOLD)
        FT_GlyphSlot_Embolden(glyph);

    /* Handle 'Italic' style and skew */
    if (glyph->format == FT_GLYPH_FORMAT_OUTLINE)
    {
        FT_Fixed slant = 0;

        if (in->skew)
            slant = FT_Tan(FT_MulDiv(in->skew, 0x10000L, 10));
        else if (in->effects & EFFECT_ITALIC)
            slant = ITALIC_SHEAR;

        if (slant)
        {
            FT_Matrix shear;

            shear.xx = 1L << 16;
            shear.xy = slant;
            shear.yx = 0;
            shear.yy = 1L << 16;

            FT_Outline_Transform(&glyph->outline, &shear);
        }
    }
    else if ((in->effects & EFFECT_ITALIC) && face->num_fixed_sizes)
    {
        bitmap_italics_shear = s->skewing;
    }

    /* FreeType fills in *aglyph only when it succeeds; every error path leaves
     * the caller's variable alone, and the errors are dominated by allocation
     * failure. */
    error = FT_Get_Glyph(glyph, &g);
    if (error)
        return error;

    /* Outlined style */
    if ((in->effects & EFFECT_OUTLINE) && g->format == FT_GLYPH_FORMAT_OUTLINE)
    {
        FT_Stroker stroker;

        error = FT_Stroker_New(library, &stroker);
        if (!error)
        {
            FT_Stroker_Set(stroker, 16, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
            error = FT_Glyph_Stroke(&g, stroker, 1 /* delete the original glyph */);
            FT_Stroker_Done(stroker);
        }
        if (error)
        {
            FT_Done_Glyph(g);
            return error;
        }
    }

    /* Get the glyph metrics if desired */
    if ((want & CACHED_METRICS) && !(cached->stored & CACHED_METRICS))
    {
        if (FT_IS_SCALABLE(face))
        {
            FT_BBox bbox;

            FT_Glyph_Get_CBox(g, FT_GLYPH_BBOX_PIXELS, &bbox);
            cached->minx = bbox.xMin;
            cached->maxx = bbox.xMax;
            cached->yoffset = s->base.font.ascent - bbox.yMax;
            cached->advance = FT_CEIL(glyph->advance.x);
        }
        else
        {
            /* Get the bounding box for non-scalable format.
             * Again, freetype2 fills in many of the font metrics
             * with the value of 0, so some of the values we
             * need must be calculated differently with certain
             * assumptions about non-scalable formats.
             */
            FT_Glyph_Metrics *metrics = &glyph->metrics;

            cached->minx = FT_FLOOR(metrics->horiBearingX);
            cached->maxx = cached->minx + FT_CEIL(metrics->horiAdvance) + bitmap_italics_shear;
            cached->yoffset = 0;
            cached->advance = FT_CEIL(metrics->horiAdvance);
        }

        cached->stored |= CACHED_METRICS;
    }

    if ((want & CACHED_BITMAP) && !(cached->stored & CACHED_BITMAP))
    {
        FT_BitmapGlyph bg;
        FT_Bitmap *src;
        UBYTE *buffer = NULL;
        LONG width, rows, pitch, row, i;

        /* Render the glyph (nothing to do for a bitmap one) */
        error = FT_Glyph_To_Bitmap(&g, FT_RENDER_MODE_MONO, NULL, 1);
        if (error)
        {
            FT_Done_Glyph(g);
            return error;
        }
        bg = (FT_BitmapGlyph)g;
        src = &bg->bitmap;

        /* Need to enlarge to fit the italics shear if bitmap font */
        width = (LONG)src->width + bitmap_italics_shear;
        rows = src->rows;
        pitch = ((width + 15) >> 4) << 1;   /* Only whole words */

        if (rows > 0 && width > 0)
        {
            LONG src_pitch = (src->pitch < 0) ? -src->pitch : src->pitch;

            /* A glyph that cannot be represented is just a missing glyph,
             * so fail this one only. */
            if (width > MAX_BITMAP || rows > MAX_BITMAP)
            {
                FT_Done_Glyph(g);
                return FT_Err_Invalid_Argument;
            }

            buffer = vdi_ft_alloc(pitch * rows);
            if (!buffer)
            {
                FT_Done_Glyph(g);
                return FT_Err_Out_Of_Memory;
            }
            ft_memset(buffer, 0, pitch * rows);

            for (row = 0; row < rows; row++)
            {
                const UBYTE *s_row = src->buffer +
                    ((src->pitch < 0) ? (rows - 1 - row) : row) * src_pitch;
                UBYTE *d_row = buffer + row * pitch;

                if (src->pixel_mode == FT_PIXEL_MODE_MONO)
                {
                    LONG n = ((LONG)src->width + 7) >> 3;

                    ft_memcpy(d_row, s_row, (n < src_pitch) ? n : src_pitch);
                }
                else if (src->pixel_mode == FT_PIXEL_MODE_GRAY)
                {
                    /* an embedded grey bitmap: keep its darker half */
                    for (i = 0; i < (LONG)src->width; i++)
                        if (s_row[i] & 0x80)
                            d_row[i >> 3] |= (UBYTE)(0x80 >> (i & 7));
                }
            }

            if (bitmap_italics_shear)
            {
                LONG distance = s->base.font.pixel_height / (bitmap_italics_shear + 1);

                if (distance < 1)
                    distance = 1;
                for (row = rows - 1; row >= 0; --row)
                {
                    LONG cnt = (s->base.font.pixel_height - cached->yoffset - row) / distance;

                    if (cnt > 0)
                        shift_row(buffer + row * pitch, pitch, cnt);
                }
            }

            /* Light/grey effect */
            if (in->effects & EFFECT_LIGHT)
            {
                for (row = rows - 1; row >= 0; --row)
                {
                    UBYTE lightening = (row & 1) ? LIGHTENING : (UBYTE)~LIGHTENING;
                    UBYTE *p = buffer + row * pitch;

                    for (i = 0; i < pitch; i++)
                        p[i] &= lightening;
                }
            }
        }

        cached->buffer = buffer;
        cached->width = buffer ? width : 0;
        cached->rows = buffer ? rows : 0;
        cached->pitch = pitch;
        cached->left = bg->left;
        cached->top = bg->top;

        /* Mark that we rendered this format */
        cached->stored |= CACHED_BITMAP;
    }

    FT_Done_Glyph(g);

    return 0;
}

static CachedGlyph *find_glyph(Instance *in, UWORD ch, LONG map, int want)
{
    CachedGlyph *glyph = (ch < 256) ? &in->cache[ch] : &in->scratch;

    /* the slot may hold another code, or this one in another character set */
    if (glyph->stored && (glyph->code != ch || glyph->map != map))
        flush_glyph(glyph);

    if ((glyph->stored & want) != want)
    {
        if (load_glyph(in, ch, map, glyph, want))
            return NULL;
    }

    return glyph;
}

/*
 * Measure a string: the extent of its glyphs from the starting pen
 * position (minx <= 0, maxx >= 0), and how far the pen moves.
 * Returns how many glyphs could not be loaded.
 */
static LONG text_size(Instance *in, const WORD *text, LONG count, LONG map,
                      LONG *pminx, LONG *pmaxx, LONG *ppen)
{
    LONG i, x = 0, z, minx = 0, maxx = 0, failed = 0;
    CachedGlyph *glyph;

    /* Load each character and sum its bounding box */
    for (i = 0; i < count; i++)
    {
        /* UNSIGNED.  A signed short makes every codepoint >= 0x8000
         * negative, and a negative value still satisfies "< 256",
         * so the cache would be indexed BEFORE its base. */
        glyph = find_glyph(in, (UWORD)text[i], map, CACHED_METRICS);
        if (!glyph)
        {
            failed++;
            continue;
        }

        z = x + glyph->minx;
        if (minx > z)
            minx = z;
        z = x + ((glyph->advance > glyph->maxx) ? glyph->advance : glyph->maxx);
        if (maxx < z)
            maxx = z;
        x += glyph->advance;

        /* past what a bitmap can hold, the rest does not matter */
        if (maxx - minx > MAX_BITMAP)
            break;
    }

    *pminx = minx;
    *pmaxx = maxx;
    *ppen = x;

    return failed;
}

/* OR a glyph into a bitmap at x, y (its top left corner), clipped */
static void blit_glyph(UBYTE *dst, LONG bytes_per_row, LONG width, LONG height,
                       const CachedGlyph *glyph, LONG x, LONG y)
{
    LONG w = glyph->width, row, i, n, shift;
    UBYTE last;

    if (x < 0)
        return;
    if (x + w > width)
        w = width - x;
    if (w <= 0)
        return;

    n = (w + 7) >> 3;
    shift = x & 7;
    last = (UBYTE)(0xff << ((8 - (w & 7)) & 7));

    for (row = 0; row < glyph->rows; row++)
    {
        const UBYTE *s = glyph->buffer + row * glyph->pitch;
        UBYTE *d;
        UBYTE carry = 0;

        if (y + row < 0 || y + row >= height)
            continue;
        d = dst + (y + row) * bytes_per_row + (x >> 3);

        for (i = 0; i < n; i++)
        {
            UBYTE b = (i == n - 1) ? (UBYTE)(s[i] & last) : s[i];

            d[i] |= (UBYTE)((b >> shift) | carry);
            carry = (UBYTE)(b << (8 - shift));
        }
        /* only pixels left of width can be in it */
        if (carry)
            d[n] |= carry;
    }
}


/*
 * ===========================================================================
 * Registering faces
 * ===========================================================================
 */

/*
 * Find a matching name/platform/encoding name for the face.
 * All matches are exact, except you can ignore encoding using -1
 */
static int find_sfnt_name(FT_Face face, int name_id, int platform_id, int encoding_id, FT_SfntName *result)
{
    FT_SfntName name;
    FT_UInt count, i;

    count = FT_Get_Sfnt_Name_Count(face);

    for (i = 0; i < count; i++)
    {
        if (FT_Get_Sfnt_Name(face, i, &name) != 0)
            continue;

        if (name.name_id == name_id &&
            name.platform_id == platform_id &&
            (encoding_id < 0 || name.encoding_id == encoding_id))
        {
            int matched = 0;

            switch (name.platform_id)
            {
            case TT_PLATFORM_MICROSOFT:
                if (name.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES ||
                    name.language_id == TT_MS_LANGID_ENGLISH_UNITED_KINGDOM)
                    matched = 1;
                break;
            case TT_PLATFORM_MACINTOSH:
            case TT_PLATFORM_APPLE_UNICODE:
                if (name.language_id == TT_MAC_LANGID_ENGLISH)
                    matched = 1;
                break;
            default:
                break;
            }
            if (matched)
            {
                *result = name;
                return 1;
            }
        }
    }
    return 0;
}

static int plain_char(UBYTE c)
{
    return (c >= 0x20 && c <= 0x7e) || c == '\r' || c == '\n' || c == '\t';
}

/* Convert Unicode (UCS-2, the Latin-1 part) to Atari ASCII (quick and dirty) */
static UBYTE latin1_to_atari(UBYTE c)
{
    if (plain_char(c))
        return c;

    switch (c)
    {
    case 0xa9:  /* copyright */
        return 189;
    case 0xae:  /* registered sign */
        return 190;
    case 0xc4:  /* capital a with diaresis */
        return 142;
    case 0xd6:  /* capital o with diaresis */
        return 153;
    case 0xdc:  /* capital u with diaresis */
        return 154;
    case 0xe4:  /* lower a with diaresis */
        return 132;
    case 0xf6:  /* lower o with diaresis */
        return 148;
    case 0xfc:  /* lower u with diaresis */
        return 129;
    default:
        return '?';
    }
}

/* Convert Mac Roman to Atari ASCII (quick and dirty) */
static UBYTE mac_roman_to_atari(UBYTE c)
{
    if (plain_char(c))
        return c;

    switch (c)
    {
    case 0xa9:  /* copyright */
        return 189;
    case 0xa8:  /* registered sign */
        return 190;
    case 0x80:  /* capital a with diaresis */
        return 142;
    case 0x85:  /* capital o with diaresis */
        return 153;
    case 0x86:  /* capital u with diaresis */
        return 154;
    case 0x8a:  /* lower a with diaresis */
        return 132;
    case 0x9a:  /* lower o with diaresis */
        return 148;
    case 0x9f:  /* lower u with diaresis */
        return 129;
    default:
        return '?';
    }
}

/*
 * Get one of the names of a face, in the Atari character set, into buf
 * (len bytes, NUL-terminated).  Returns 0 if the face has no such name.
 */
static int get_name(FT_Face face, int name_id, char *buf, LONG len)
{
    FT_SfntName name;
    LONG i, n = 0;

    /* Most fonts are MICROSOFT + UNICODE + (english) so check that first */
    if (find_sfnt_name(face, name_id, TT_PLATFORM_MICROSOFT, TT_MS_ID_UNICODE_CS, &name) ||
        find_sfnt_name(face, name_id, TT_PLATFORM_APPLE_UNICODE, -1, &name))
    {
        for (i = 0; i + 1 < (LONG)name.string_len && n < len - 1; i += 2)
            buf[n++] = (char)(name.string[i] ? '?' : latin1_to_atari(name.string[i + 1]));
    }
    /* Very old Apple fonts might be this */
    else if (find_sfnt_name(face, name_id, TT_PLATFORM_MACINTOSH, TT_MAC_ID_ROMAN, &name))
    {
        for (i = 0; i < (LONG)name.string_len && n < len - 1; i++)
            buf[n++] = (char)mac_roman_to_atari(name.string[i]);
    }
    else
        return 0;

    buf[n] = 0;
    return 1;
}

static void append(char *buf, LONG len, const char *s)
{
    LONG n = ft_strlen(buf);

    while (s && *s && n < len - 1)
        buf[n++] = *s++;
    buf[n] = 0;
}

static void append_number(char *buf, LONG len, LONG value)
{
    char digits[12];
    LONG n = sizeof(digits) - 1;

    digits[n] = 0;
    if (value < 0)
        value = 0;
    do
    {
        digits[--n] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n > 0);
    append(buf, len, digits + n);
}

/* the family_name + style_name hashcode, as fVDI computes it */
static LONG face_id(FT_Face face)
{
    const char *p;
    UWORD hc = 0;
    LONG id;
    FaceRec *f;

    for (p = face->family_name; p && *p; p++)
        hc = (UWORD)(hc * 31 + (UWORD)(SBYTE)*p);
    for (p = face->style_name; p && *p; p++)
        hc = (UWORD)(hc * 31 + (UWORD)(SBYTE)*p);

    /* id is >5000 for Vector fonts (from SpeedoGDOS)  */
    id = (WORD)((WORD)hc + 5000);
    if (id < 0)
        id = -id;
    if (id < 5000)
        id += 5000;
    if (id > 32767)
        id = 5000;

    /* Find out whether there already is such a font ID */
    for (;;)
    {
        id++;
        if (id > 32767)
            id = 5001;

        for (f = faces; f; f = f->next)
            if (f->base.font.id == id)
                break;
        if (!f)
            return id;
    }
}

/*
 * Classify a face from the tables FreeType has already parsed.
 *
 * The OS/2 family class is the authoritative answer where it exists:
 * its high byte is the IBM font class, 1-7 being the serif families,
 * 8 sans serif, 9 ornamental, 10 script and 12 symbolic.  Class 6 is
 * reserved and 0 means the font declined to say, so both fall through.
 *
 * PANOSE is the fallback, because plenty of faces leave sFamilyClass at
 * zero but fill PANOSE in.  Its first byte is the family kind and, for
 * Latin text, the second is the serif style: 2-10 are the serif shapes
 * and 11-15 the sans ones.
 *
 * Neither is a measurement, so both can be wrong if the font lies about
 * itself.  That is why the raw bytes go back to the caller as well as
 * the cooked answer.
 */
static WORD classify(const TT_OS2 *os2)
{
    switch ((os2->sFamilyClass >> 8) & 0xff)
    {
    case 1: case 2: case 3: case 4: case 5: case 7:
        return VDI_FCLASS_SERIF;
    case 8:
        return VDI_FCLASS_SANS;
    case 9:
        return VDI_FCLASS_DECORATIVE;
    case 10:
        return VDI_FCLASS_SCRIPT;
    case 12:
        return VDI_FCLASS_SYMBOL;
    default:
        break;
    }

    switch (os2->panose[0])
    {
    case 2:     /* Latin text */
        if (os2->panose[1] >= 2 && os2->panose[1] <= 10)
            return VDI_FCLASS_SERIF;
        if (os2->panose[1] >= 11 && os2->panose[1] <= 15)
            return VDI_FCLASS_SANS;
        break;
    case 3:     /* Latin hand written */
        return VDI_FCLASS_SCRIPT;
    case 4:     /* Latin decorative */
        return VDI_FCLASS_DECORATIVE;
    case 5:     /* Latin symbol */
        return VDI_FCLASS_SYMBOL;
    default:
        break;
    }

    return VDI_FCLASS_UNKNOWN;
}

/* is a name empty or all spaces? */
static int blank(const char *s)
{
    for (; s && *s; s++)
        if (*s != ' ')
            return 0;
    return 1;
}

/* does the glyph in the slot show anything? */
static int has_ink(FT_GlyphSlot slot)
{
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
        return slot->outline.n_points > 0;
    if (slot->format == FT_GLYPH_FORMAT_BITMAP)
        return slot->bitmap.rows > 0 && slot->bitmap.width > 0;
    return 0;
}

/* load a glyph: 1 if it shows something, 0 if not, -1 if out of memory */
static LONG try_glyph(FT_Face face, FT_UInt index)
{
    FT_Error error = FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING);

    if (error == FT_Err_Out_Of_Memory)
        return -1;
    return !error && has_ink(face->glyph);
}

/*
 * A font file cut short often still opens, and gives a face whose glyphs
 * are missing or empty: it would be listed, and draw nothing.  So a face
 * must show 'A' at a small size, or one of its first characters, or,
 * without a character map, one of its first glyphs.
 *
 * Returns 0 if it does, -2 if not, -1 if out of memory.
 */
static LONG check_glyphs(FT_Face face)
{
    FT_ULong code;
    FT_UInt index;
    LONG i, r;

    if (FT_IS_SCALABLE(face))
    {
        if (FT_Set_Pixel_Sizes(face, 0, 12))
            return -2;
    }
    else if (face->num_fixed_sizes <= 0 || FT_Select_Size(face, 0))
        return -2;

    index = FT_Get_Char_Index(face, 'A');
    if (index && (r = try_glyph(face, index)) != 0)
        return (r > 0) ? 0 : -1;

    code = FT_Get_First_Char(face, &index);
    for (i = 0; index && i < GLYPH_TRIES; i++)
    {
        if ((r = try_glyph(face, index)) != 0)
            return (r > 0) ? 0 : -1;
        code = FT_Get_Next_Char(face, code, &index);
    }

    for (index = 1; index < (FT_UInt)face->num_glyphs && index <= GLYPH_TRIES; index++)
        if ((r = try_glyph(face, index)) != 0)
            return (r > 0) ? 0 : -1;

    return -2;
}

/*
 * register a face that is open; returns 0, -1 if out of memory, or -2 if
 * the face has no name or draws nothing
 */
static LONG add_face(FT_Face face, const char *path, LONG index)
{
    FaceRec *fr, **pp;
    VdiOutlineFont *font;
    TT_OS2 *os2;
    char name[sizeof(font->name)];
    LONG i;

    /* Best practice conveniently keeps this <= 32 chars */
    name[0] = 0;
    if (!get_name(face, TT_NAME_ID_FULL_NAME, name, sizeof(name)))
    {
        /* as the above function just sets the name from TTF fonts, we still do this */
        append(name, sizeof(name), face->family_name);
        append(name, sizeof(name), " ");
        append(name, sizeof(name), face->style_name);
        if (face->num_fixed_sizes > 0)
        {
            append(name, sizeof(name), " ");
            append_number(name, sizeof(name), face->available_sizes[0].height);
        }
    }

    /* a face that could not be told from others in a list */
    if (blank(name) || blank(face->family_name))
        return -2;

    i = check_glyphs(face);
    if (i < 0)
        return i;

    fr = vdi_ft_alloc(sizeof(FaceRec) + ft_strlen(path));
    if (!fr)
        return -1;
    ft_memset(fr, 0, sizeof(FaceRec));
    ft_strcpy(fr->path, path);
    font = &fr->base.font;
    ft_memcpy(font->name, name, sizeof(font->name));

    font->id = face_id(face);
    font->flags = (FT_IS_SCALABLE(face) ? VDI_FT_SCALABLE : 0) |
                  (FT_IS_FIXED_WIDTH(face) ? VDI_FT_MONOSPACE : 0);
    /* Vector fonts have size = 0 */
    font->points = (face->num_fixed_sizes && !FT_IS_SCALABLE(face)) ?
                   face->available_sizes[0].size / 64 : 0;
    font->first_ade = 0;
    font->last_ade = 255;
    font->private = &fr->base;
    fr->base.kind = KIND_FACE;
    fr->index = index;

    append(fr->family, sizeof(fr->family), face->family_name);
    append(fr->style, sizeof(fr->style), face->style_name);
    if (!get_name(face, TT_NAME_ID_PS_NAME, fr->ps_name, sizeof(fr->ps_name)))
        fr->ps_name[0] = 0;
    fr->num_glyphs = face->num_glyphs;
    fr->units_per_em = face->units_per_EM;
    fr->style_flags = face->style_flags;

    os2 = (TT_OS2 *)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    /* FreeType reports a missing OS/2 table as version 0xffff
     * rather than a NULL pointer. */
    if (os2 && os2->version != 0xffff)
    {
        fr->os2_valid = 1;
        fr->family_class = os2->sFamilyClass;
        for (i = 0; i < 10; i++)
            fr->panose[i] = os2->panose[i];
        fr->font_class = classify(os2);
    }

    for (pp = &faces; *pp; pp = &(*pp)->next)
        ;
    *pp = fr;
    face_count++;

    return 0;
}

/*
 * Case, and which slash, do not make two paths different files (as fVDI's
 * same_path()): a folder scanned again can give its names in another case
 * than the caller, under MiNT for one.
 */
static int same_path(const char *a, const char *b)
{
    char ca, cb;

    do
    {
        ca = *a++;
        cb = *b++;
        if (ca == '/')
            ca = '\\';
        if (cb == '/')
            cb = '\\';
        if (ca >= 'a' && ca <= 'z')
            ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z')
            cb -= 'a' - 'A';
        if (ca != cb)
            return 0;
    } while (ca);

    return 1;
}

/* is this the name of an NVDI/SpeedoGDOS character map file? */
static int is_spdchar_map(const char *path)
{
    const char *name = path, *p;
    const char *want = "spdchar.map";

    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            name = p + 1;

    for (; *name && *want; name++, want++)
    {
        char c = *name;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != *want)
            return 0;
    }

    return !*name && !*want;
}

static LONG load_spdchar_map(const char *path)
{
    UBYTE *data;
    ULONG size = 0;
    LONG i;

    data = vdi_font_file_load(path, &size);
    if (!data)
        return -1;

    /* the first table is an id word and 224 entries, all big-endian */
    spdchar_map_valid = (size >= 450);
    if (spdchar_map_valid)
        for (i = 0; i < 224; i++)
            spdchar_map[i] = (WORD)((data[2 + 2 * i] << 8) | data[3 + 2 * i]);
    vdi_font_file_free(data);

    /* glyphs looked up through the old map are out of date */
    flush_all_caches();

    return 0;
}


/*
 * ===========================================================================
 * Sizes
 * ===========================================================================
 */

/* the registered face of a face or size, and the size if it is one */
static FaceRec *face_of(const VdiOutlineFont *f, SizeRec **size)
{
    FontBase *b;

    if (size)
        *size = NULL;
    if (!f)
        return NULL;
    b = f->private;
    if (!b || &b->font != f)
        return NULL;
    if (b->kind == KIND_FACE)
        return (FaceRec *)b;
    if (b->kind == KIND_SIZE)
    {
        if (size)
            *size = (SizeRec *)b;
        return ((SizeRec *)b)->face;
    }
    return NULL;
}

static SizeRec *get_size(FaceRec *fr, LONG points, LONG pixels, LONG char_width)
{
    SizeRec *s;

    if (points <= 0)
        points = DEFAULT_POINTS;
    if (points > MAX_SIZE)
        points = MAX_SIZE;
    if (pixels <= 0)
        pixels = points;
    if (pixels > MAX_SIZE)
        pixels = MAX_SIZE;
    if (char_width < 0 || char_width > MAX_SIZE || !(fr->base.font.flags & VDI_FT_SCALABLE))
        char_width = 0;

    for (s = fr->sizes; s; s = s->next)
        if (s->points == points && s->pixels == pixels && s->char_width == char_width)
            return s;

    s = vdi_ft_alloc(sizeof(SizeRec));
    if (!s)
        return NULL;
    ft_memset(s, 0, sizeof(SizeRec));
    s->base.font = fr->base.font;
    s->base.font.private = &s->base;
    s->base.kind = KIND_SIZE;
    s->face = fr;
    s->points = points;
    s->pixels = pixels;
    s->char_width = char_width;

    /* NEED to update metrics to be up-to-date immediately after vst_point() */
    if (!open_instance(s, 0, 0))
    {
        vdi_ft_free(s);
        return NULL;
    }

    s->next = fr->sizes;
    fr->sizes = s;

    return s;
}

/* the size to draw a face or size with; NULL if there is none */
static SizeRec *size_of(VdiOutlineFont *f)
{
    SizeRec *s;
    FaceRec *fr = face_of(f, &s);

    if (!fr || !library)
        return NULL;
    if (!s)
        s = get_size(fr, DEFAULT_POINTS, DEFAULT_POINTS, 0);

    return s;
}


/*
 * ===========================================================================
 * The boundary functions
 * ===========================================================================
 */

/*
 * start FreeType
 *
 * returns 0 if all is well, or a negative number
 */
LONG vdi_ft_init(void)
{
    FT_Error error;
    unsigned int i;

    if (library)
        return 0;

    error = FT_Init_FreeType(&library);
    if (error)
    {
        library = NULL;
        return -(LONG)error;
    }

    /* FreeType carries on when a module cannot be added for lack of
     * memory, but without one of them no font would ever open */
    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (!FT_Get_Module(library, modules[i]))
        {
            FT_Done_FreeType(library);
            library = NULL;
            return -(LONG)FT_Err_Out_Of_Memory;
        }
    }

    return 0;
}


void vdi_ft_term(void)
{
    /* Cleanup the FT2 font cache */
    while (faces)
    {
        FaceRec *fr = faces;

        faces = fr->next;
        while (fr->sizes)
        {
            SizeRec *s = fr->sizes;

            fr->sizes = s->next;
            while (s->instances)
                close_instance(s->instances);
            vdi_ft_free(s);
        }
        vdi_ft_free(fr);
    }
    face_count = 0;

    while (fontfiles)
    {
        FontFile *f = fontfiles;

        fontfiles = f->next;
        vdi_font_file_free(f->data);
        vdi_ft_free(f);
    }
    spdchar_map_valid = 0;

    /* Terminate the FT2 library */
    if (library)
    {
        FT_Done_FreeType(library);
        library = NULL;
    }
}


/*
 * Register the faces of a font file.  The file is read to find them and
 * their names, and is not kept: it is read again when a face is first
 * sized.
 *
 * Faces without a name, or whose glyphs are missing or empty (a file cut
 * short often still opens), are left out.
 *
 * Returns the number of faces added (0 if the file is registered already,
 * or is an NVDI SPDCHAR.MAP, which is loaded), or a negative number if the
 * file cannot be read (-1), has no usable face (-2) or there is not enough
 * memory (-3).
 */
LONG vdi_ft_register_file(const char *path)
{
    FaceRec *fr;
    FT_Face face;
    void *data;
    ULONG size = 0;
    LONG i, r, num, added = 0, result = 0;

    if (!library || !path)
        return -1;

    if (is_spdchar_map(path))
        return load_spdchar_map(path);

    for (fr = faces; fr; fr = fr->next)
        if (same_path(fr->path, path))
            return 0;

    data = vdi_font_file_load(path, &size);
    if (!data)
        return -1;

    /* Open the font and create ancillary data */
    if (FT_New_Memory_Face(library, data, size, 0, &face))
    {
        vdi_font_file_free(data);
        return -2;
    }

    num = face->num_faces;
    if (num > MAX_FILE_FACES)
        num = MAX_FILE_FACES;

    for (i = 0; i < num; i++)
    {
        if (i > 0 && FT_New_Memory_Face(library, data, size, i, &face))
            continue;

        r = add_face(face, path, i);
        FT_Done_Face(face);

        if (r == 0)
            added++;
        else if (r == -1)
        {
            result = -3;
            break;
        }
    }

    vdi_font_file_free(data);

    return added ? added : (result ? result : -2);
}


LONG vdi_ft_face_count(void)
{
    return face_count;
}


VdiOutlineFont *vdi_ft_face(LONG index)
{
    FaceRec *fr;

    if (index < 0)
        return NULL;

    for (fr = faces; fr && index > 0; fr = fr->next)
        index--;

    return fr ? &fr->base.font : NULL;
}


VdiOutlineFont *vdi_ft_size(VdiOutlineFont *face, LONG points, LONG pixel_height,
                            LONG char_width)
{
    FaceRec *fr = face_of(face, NULL);
    SizeRec *s;

    if (!fr || !library)
        return NULL;

    s = get_size(fr, points, pixel_height, char_width);

    return s ? &s->base.font : NULL;
}


LONG vdi_ft_text_extent(VdiOutlineFont *f, const WORD *text, LONG count,
                        LONG map, LONG effects, LONG *width, LONG *height)
{
    SizeRec *s = size_of(f);
    Instance *in;
    LONG minx, maxx, pen, w;

    if (width)
        *width = 0;
    if (height)
        *height = 0;
    if (!s)
        return -1;
    in = get_instance(s, effects, 0);
    if (!in)
        return -1;
    if (!text || count < 0)
        count = 0;

    text_size(in, text, count, map, &minx, &maxx, &pen);

    /* a monospaced font is as wide as its advances add up to */
    w = FT_IS_FIXED_WIDTH(in->face) ? pen : maxx - minx;

    if (width)
        *width = w;
    if (height)
        *height = s->base.font.pixel_height;

    return 0;
}


/*
 * advance[0] is the horizontal advance of the character, in pixels;
 * advance[1] is the vertical one, and advance[2] and [3] the remainders,
 * all 0 here.
 */
LONG vdi_ft_char_advance(VdiOutlineFont *f, LONG ch, LONG map, LONG effects,
                         LONG *advance)
{
    SizeRec *s = size_of(f);
    Instance *in = s ? get_instance(s, effects, 0) : NULL;
    CachedGlyph *glyph = in ? find_glyph(in, (UWORD)ch, map, CACHED_METRICS) : NULL;

    if (advance)
    {
        advance[0] = glyph ? glyph->advance : 0;
        advance[1] = 0;
        advance[2] = 0;
        advance[3] = 0;
    }

    return glyph ? 0 : -1;
}


LONG vdi_ft_render(VdiOutlineFont *f, const WORD *text, LONG count,
                   LONG map, LONG effects, LONG skew, VdiOutlineBitmap *out)
{
    SizeRec *s;
    Instance *in;
    UBYTE *data;
    LONG minx, maxx, pen, width, height, bytes_per_row, i, failed;
    CachedGlyph *glyph;

    if (!out)
        return -1;
    ft_memset(out, 0, sizeof(*out));

    if (!text || count <= 0)
        return 0;

    s = size_of(f);
    if (!s)
        return -1;
    in = get_instance(s, effects, skew);
    if (!in)
        return -1;

    /* Get the dimensions of the text surface */
    failed = text_size(in, text, count, map, &minx, &maxx, &pen);
    width = maxx - minx;
    if (width <= 0)
        return failed ? -1 : 0;
    height = s->base.font.pixel_height;

    /* The VDI's forms are measured in 16-bit numbers: refuse to render
     * a run it could not draw instead of handing it a wrapped size. */
    if (width > MAX_BITMAP || height > MAX_BITMAP)
        return -1;

    bytes_per_row = ((width + 15) >> 4) * 2;
    data = vdi_ft_alloc(bytes_per_row * height);
    if (!data)
        return -1;
    ft_memset(data, 0, bytes_per_row * height);

    /* Load and render each character */
    pen = 0;
    for (i = 0; i < count; i++)
    {
        glyph = find_glyph(in, (UWORD)text[i], map, CACHED_METRICS | CACHED_BITMAP);
        if (!glyph)
            continue;

        if (glyph->buffer)
            blit_glyph(data, bytes_per_row, width, height, glyph,
                       pen + glyph->left - minx, s->base.font.top - glyph->top);
        pen += glyph->advance;

        /* only when a glyph failed to load while measuring */
        if (pen > maxx)
            break;
    }

    /* Handle the underline style */
    if (effects & EFFECT_UNDERLINE)
    {
        LONG row = s->base.font.ascent - s->base.font.underline_offset - 1;
        LONG k, n = width >> 3;
        UBYTE color = 0xff;

        if (row + s->base.font.underline_size > height)
            row = (height - 1) - s->base.font.underline_size;
        if (row < 0)
            row = 0;

        for (k = s->base.font.underline_size; k > 0 && row < height; --k, row++)
        {
            UBYTE *p = data + row * bytes_per_row;

            /* 1 because 0 is the bg color */
            if (in->effects & EFFECT_LIGHT)
                color = (k & 1) ? LIGHTENING : (UBYTE)~LIGHTENING;
            ft_memset(p, color, n);
            if (width & 7)
                p[n] |= (UBYTE)(color & (0xff << (8 - (width & 7))));
        }
    }

    out->data = (UWORD *)data;
    out->width = width;
    out->height = height;
    out->wdwidth = bytes_per_row / 2;
    out->origin_x = -minx;
    out->origin_y = s->base.font.top;
    out->advance_x = pen;

    return 0;
}


void vdi_ft_release(VdiOutlineBitmap *bitmap)
{
    if (bitmap && bitmap->data)
    {
        vdi_ft_free(bitmap->data);
        bitmap->data = NULL;
    }
}


LONG vdi_ft_char_index(VdiOutlineFont *f, LONG ch, LONG from_mode, LONG to_mode)
{
    SizeRec *s = size_of(f);
    Instance *in = s ? get_instance(s, 0, 0) : NULL;

    if (!in)
        return 0xffff;

    return map_char(in->face, (UWORD)ch, from_mode, to_mode);
}


/*
 * A character's outline as GEM Bezier data, for v_getoutline and
 * v_get_outline: points in whole pixels, x from the character origin and
 * y UP from the baseline (the caller places them), with v_bez's flags -
 * bit 0 starts a four-point Bezier, bit 1 is a jump not joined to the
 * point before.  Each contour starts with a jump and is closed.
 *
 * The glyph is loaded unhinted, so the outline is the font's own shape at
 * the current size rather than the one fitted to the pixel grid.
 * TrueType's quadratic segments become cubics with the same curve: the
 * two controls lie two thirds of the way from each end to the quadratic
 * control point.
 */

typedef struct
{
    WORD *xy;               /* caller's points */
    BYTE *bez;              /* caller's v_bez flags */
    LONG max;               /* room for this many points */
    LONG count;             /* points written */
    LONG overflow;          /* ran out of room */
    FT_Pos last_x, last_y;  /* FreeType's working state, 26.6 */
    FT_Pos start_x, start_y;
    LONG open;
} OutlineArgs;

static LONG px(FT_Pos v)
{
    return v >= 0 ? (v + 32) >> 6 : -((-v + 32) >> 6);
}

static void outline_point(OutlineArgs *o, FT_Pos x, FT_Pos y, BYTE flag)
{
    if (o->count >= o->max)
    {
        o->overflow = 1;
        return;
    }
    o->xy[o->count * 2] = (WORD)px(x);
    o->xy[o->count * 2 + 1] = (WORD)px(y);
    o->bez[o->count] = flag;
    o->count++;
}

static void outline_close(OutlineArgs *o)
{
    if (o->open && (o->last_x != o->start_x || o->last_y != o->start_y))
        outline_point(o, o->start_x, o->start_y, 0);
    o->open = 0;
}

static int outline_move(const FT_Vector *to, void *user)
{
    OutlineArgs *o = user;

    outline_close(o);
    outline_point(o, to->x, to->y, 2);          /* jump */
    o->start_x = o->last_x = to->x;
    o->start_y = o->last_y = to->y;
    o->open = 1;
    return o->overflow;
}

static int outline_line(const FT_Vector *to, void *user)
{
    OutlineArgs *o = user;

    outline_point(o, to->x, to->y, 0);
    o->last_x = to->x;
    o->last_y = to->y;
    return o->overflow;
}

static int outline_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user)
{
    OutlineArgs *o = user;

    if (o->count > 0 && o->count <= o->max)
        o->bez[o->count - 1] |= 1;              /* the last point starts this Bezier */
    outline_point(o, c1->x, c1->y, 0);
    outline_point(o, c2->x, c2->y, 0);
    outline_point(o, to->x, to->y, 0);
    o->last_x = to->x;
    o->last_y = to->y;
    return o->overflow;
}

static int outline_conic(const FT_Vector *control, const FT_Vector *to, void *user)
{
    OutlineArgs *o = user;
    FT_Vector c1, c2;

    c1.x = o->last_x + (2 * (control->x - o->last_x)) / 3;
    c1.y = o->last_y + (2 * (control->y - o->last_y)) / 3;
    c2.x = to->x + (2 * (control->x - to->x)) / 3;
    c2.y = to->y + (2 * (control->y - to->y)) / 3;

    return outline_cubic(&c1, &c2, to, user);
}

/* Returns the number of points, 0 for no outline, -1 when max is too small */
LONG vdi_ft_get_outline(VdiOutlineFont *f, LONG ch, LONG map, WORD *xy, BYTE *flags,
                        LONG max_points)
{
    SizeRec *s;
    Instance *in;
    FT_Outline_Funcs funcs;
    FT_Face face;
    FT_UInt glyph;
    OutlineArgs o;
    LONG code;

    if (!xy || !flags || max_points <= 0)
        return 0;

    s = size_of(f);
    in = s ? get_instance(s, 0, 0) : NULL;
    if (!in)
        return 0;
    face = in->face;
    if (!FT_IS_SCALABLE(face))
        return 0;

    code = map_char(face, (UWORD)ch, map, VDI_FT_MAP_UNICODE);
    if (code == 0xffff || (glyph = FT_Get_Char_Index(face, code)) == 0)
        return 0;

    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return 0;

    ft_memset(&o, 0, sizeof(o));
    o.xy = xy;
    o.bez = flags;
    o.max = max_points;

    funcs.move_to = outline_move;
    funcs.line_to = outline_line;
    funcs.conic_to = outline_conic;
    funcs.cubic_to = outline_cubic;
    funcs.shift = 0;
    funcs.delta = 0;
    FT_Outline_Decompose(&face->glyph->outline, &funcs, &o);
    outline_close(&o);

    return o.overflow ? -1 : o.count;
}


void vdi_ft_xfntinfo(VdiOutlineFont *f, LONG flags, void *xfnt_info)
{
    FaceRec *fr = face_of(f, NULL);
    VdiXfntInfo *info = xfnt_info;
    LONG i;

    if (!fr || !info)
        return;

    if (flags & VDI_XFNT_FONT_NAME)
    {
        for (i = 0; i < 32; i++)
            info->font_name[i] = fr->base.font.name[i];
        info->font_name[i] = 0;
    }

    if (flags & VDI_XFNT_FAMILY_NAME)
    {
        ft_strncpy(info->family_name, fr->family, sizeof(info->family_name) - 1);
        info->family_name[sizeof(info->family_name) - 1] = 0;
    }

    if (flags & VDI_XFNT_STYLE_NAME)
    {
        ft_strncpy(info->style_name, fr->style, sizeof(info->style_name) - 1);
        info->style_name[sizeof(info->style_name) - 1] = 0;
    }

    if (flags & VDI_XFNT_FILE_NAME1)
    {
        ft_strncpy(info->file_name1, fr->path, sizeof(info->file_name1) - 1);
        info->file_name1[sizeof(info->file_name1) - 1] = 0;
    }

    if (flags & VDI_XFNT_FILE_NAME2)
        info->file_name2[0] = 0;

    if (flags & VDI_XFNT_FILE_NAME3)
        info->file_name3[0] = 0;

    /* 0x100 is without enlargement, 0x200 with; bounded by the destination */
    if (flags & (VDI_XFNT_SIZES | VDI_XFNT_SIZES2))
    {
        for (i = 0; i < (LONG)(sizeof(pt_sizes) / sizeof(pt_sizes[0])) &&
                    i < (LONG)(sizeof(info->pt_sizes) / sizeof(info->pt_sizes[0])); i++)
            info->pt_sizes[i] = pt_sizes[i];
        info->pt_cnt = (WORD)i;
    }

    if (flags & VDI_XFNT_CLASS)
    {
        info->font_class = VDI_FCLASS_UNKNOWN;
        info->font_flags = VDI_FFLAG_VALID;
        info->family_class = 0;
        for (i = 0; i < 10; i++)
            info->panose[i] = 0;

        /* Measured by FreeType from the face itself rather than read
         * out of a table, so it is right for the non-sfnt formats
         * too. */
        if (fr->base.font.flags & VDI_FT_MONOSPACE)
            info->font_flags |= VDI_FFLAG_MONOSPACED;

        if (fr->os2_valid)
        {
            info->family_class = fr->family_class;
            for (i = 0; i < 10; i++)
                info->panose[i] = fr->panose[i];
            info->font_class = fr->font_class;
        }
    }
}


void vdi_ft_fontheader(VdiOutlineFont *f, void *vqt_fhdr)
{
    FaceRec *fr = face_of(f, NULL);
    VdiFontHeader *fhdr = vqt_fhdr;

    if (!fr || !fhdr)
        return;

    /* Strings should not have NUL termination if max size. */
    /* Normally 1000 ORUs per Em square (width of 'M'), but header says. */
    ft_memcpy(fhdr->fh_fmver, "D1.0\x0d\x0a\0\0", 8);  /* Format identifier */
    fhdr->fh_fntsz = 0;     /* Font file size */
    fhdr->fh_fbfsz = 0;     /* Minimum font buffer size (non-image data) */
    fhdr->fh_cbfsz = 0;     /* Minimum character buffer size (largest char) */
    fhdr->fh_hedsz = sizeof(VdiFontHeader);  /* Header size */
    fhdr->fh_fntid = 0;     /* Font ID (Bitstream) */
    fhdr->fh_sfvnr = 0;     /* Font version number */
    ft_memcpy(fhdr->fh_fntnm, fr->base.font.name, sizeof(fr->base.font.name));  /* vqt_name */
    fhdr->fh_fntnm[sizeof(fr->base.font.name)] = 0;
    fhdr->fh_mdate[0] = 0;  /* Manufacturing date (DD Mon YY) */
    fhdr->fh_laynm[0] = 0;  /* Character set name, vendor ID, character set ID */
    fhdr->fh_cpyrt[0] = 0;  /* Copyright notice */
    fhdr->fh_nchrl = 0;     /* Number of character indices in character set */
    fhdr->fh_nchrf = (UWORD)fr->num_glyphs;  /* Total number of character indices in font */
    fhdr->fh_fchrf = 0;     /* Index of first character */
    fhdr->fh_nktks = 0;     /* Number of kerning tracks */
    fhdr->fh_nkprs = 0;     /* Number of kerning pairs */
    fhdr->fh_flags = 0;     /* Font flags, bit 0 - extended mode */

    /* Classification flags: bit 0 - Italic, bit 1 - Monospace */
    fhdr->fh_cflgs = 0;
    if (fr->style_flags & FT_STYLE_FLAG_ITALIC)
        fhdr->fh_cflgs |= 1;
    if (fr->base.font.flags & VDI_FT_MONOSPACE)
        fhdr->fh_cflgs |= 2;

    /* Family classification */
    fhdr->fh_famcl = 0;
    if (fr->base.font.flags & VDI_FT_MONOSPACE)
        fhdr->fh_famcl |= 8;

    /* Font form classification: 0x_8 normal width, 0x6_ normal weight, 0xa_ bold */
    fhdr->fh_frmcl = 0x68;
    if (fr->style_flags & FT_STYLE_FLAG_BOLD)
        fhdr->fh_frmcl = (char)((fhdr->fh_frmcl & 0x0f) | 0xa0);

    /* Postscript font name */
    ft_strncpy(fhdr->fh_sfntn, fr->ps_name, sizeof(fhdr->fh_sfntn));

    /* Abbreviation of the typeface family name */
    ft_strncpy(fhdr->fh_sfacn, fr->family, sizeof(fhdr->fh_sfacn));

    /* The below should likely include "Italic" etc */
    ft_strncpy(fhdr->fh_fntfm, fr->style, sizeof(fhdr->fh_fntfm));

    fhdr->fh_itang = 0;     /* Italic angle */
    fhdr->fh_orupm = (UWORD)fr->units_per_em;  /* ORUs per Em */
}
