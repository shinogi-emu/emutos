/*
 * vdi_textblit.c - the text_blt() mainline code
 *
 * Copyright (C) 2017-2025 The EmuTOS development team
 *
 * Authors:
 *  RFB   Roger Burrows
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "asm.h"
#include "intmath.h"

#include "tosvars.h"
#include "vdi_defs.h"
#include "vdistub.h"
#include "lineavars.h"
#include "vdi_inline.h"
#include "biosext.h"


/*
 * the following structure mimics the format of the stack frame
 * containing the local variables used by the lower-level assembler
 * routines.  comments are taken from the assembler source.
 *
 * this could (and should) be cleaned up at some time, but any changes
 * MUST be synchronised with corresponding changes to the assembler code.
 */
typedef struct {
                        /* temporary working variables */
    WORD unused5;           /* was chup_flag (=CHUP-1800) */
    WORD blt_flag;
    WORD unused1;           /* was tmp_style */
                        /* working copies of the clipping variables */
    WORD unused6;           /* was YMX_CLIP */
    WORD unused7;           /* was XMX_CLIP */
    WORD unused8;           /* was YMN_CLIP */
    WORD unused9;           /* was XMN_CLIP */
    WORD unused10;          /* was CLIP */
                        /* working copies of often-used globals */
    WORD unused11;          /* was CHUP */
    WORD DESTY;
    WORD DELY;
    WORD DESTX;
    WORD DELX;
    WORD unused3;           /* was SKEWMASK */
    WORD WRT_MODE;
    WORD STYLE;
                        /* temps for arbitrary text scaling */
    WORD swap_tmps;         /* nonzero if temps are swapped */
    WORD tmp_dely;          /* temp DELY,DELX used by scaling */
    WORD tmp_delx;
                        /* colour, planes, etc */
    WORD nextwrd;           /* offset to next word in same plane */
    WORD nbrplane;          /* # planes */
    WORD forecol;           /* foreground colour */
                        /* masks for special effects */
    WORD thknover;          /* overflow for word thicken */
    WORD skew_msk;          /* rotate this to check shift */
    WORD lite_msk;          /* AND with this to get light effect */
    WORD ambient;           /* background colour */
    WORD smear;             /* amount to increase width */
                        /* vectors that may contain twoptable entries */
    void *litejpw;          /* vector for word function after lighten */
    void *thknjpw;          /* vector for word function after thicken */
                        /* vectors that may contain a toptable entry */
    void *litejpwf;         /* vector for word fringe function after lighten */
    void *thknjpwf;         /* vector for word fringe function after thicken */
    void *skewjmp;          /* vector for function after skew */
    void *litejmp;          /* vector for function after lighten */
    void *thknjmp;          /* vector for function after thicken */
                        /* other general-usage stuff */
    WORD wrd_cnt;           /* number inner loop words for left/right */
    WORD shif_cnt;          /* shift count for use by left/right shift routines */
    WORD rota_msk;          /* overlap between words in inner loop */
    WORD left_msk;          /* fringes of destination to be affected */
    WORD rite_msk;
    WORD thk_msk;           /* right fringe mask, before thicken */
    WORD src_wthk;
    WORD src_wrd;           /* # full words between fringes (source) (before thicken) */
    WORD dest_wrd;          /* # full words between fringes (destination) */
    WORD tddad;             /* destination dot address */
    WORD tsdad;             /* source dot address (pixel address, 0-15 word offset) */
    WORD height;            /* height of area in pixels */
    WORD width;             /* width of area in pixels */
    WORD d_next;            /* width of dest form (_v_lin_wr formerly used) */
    WORD s_next;            /* width of source form (formerly s_width) */
    UBYTE *dform;           /* start of destination form */
    UBYTE *sform;           /* start of source form */
    WORD unused2;           /* was buffc */
    WORD unused4;           /* was buffb */
    WORD buffa;             /* for clip & prerotate blt */
} LOCALVARS;

/* here we should have the preprocessor verify the length of LOCALVARS */

/*
 * assembler functions in vdi_tblit.S
 */
void normal_blit(LOCALVARS *vars, UBYTE *src, UBYTE *dst);
void outline(LOCALVARS *vars);
void rotate(LOCALVARS *vars);   /* actually local, but non-static improves performance */
void scale(LOCALVARS *vars);

/*
 * the following table maps a 4-bit sequence to its reverse
 */
static UBYTE reverse_nybble[] =
/*  0000  0001  0010  0011  0100  0101  0110  0111  */
{   0x00, 0x08, 0x04, 0x0c, 0x02, 0x0a, 0x06, 0x0e,
/*  1000  1001  1010  1011  1100  1101  1110  1111  */
    0x01, 0x09, 0x05, 0x0d, 0x03, 0x0b, 0x07, 0x0f };


/*
 * check for clipping
 *
 * returns  1 clipping required
 *          0 no clipping
 *         -1 entirely clipped, nothing to output
 */
static WORD check_clip(LOCALVARS *vars, WORD delx, WORD dely)
{
    WORD rc;

    if (!CLIP)
        return 0;

    rc = 0;

    /*
     * check x coordinate
     */
    if (vars->DESTX < XMINCL)               /* (partially) left of clip window */
    {
        if (vars->DESTX+delx <= XMINCL)     /* wholly left of clip window */
            return -1;
        rc = 1;
    }
    if (vars->DESTX > XMAXCL)               /* wholly right of clip window */
        return -1;
    if (vars->DESTX+delx > XMAXCL)          /* partially right of clip window */
        rc = 1;

    /*
     * check y coordinate
     */
    if (vars->DESTY < YMINCL)               /* (partially) below clip window */
    {
        if (vars->DESTY+dely <= YMINCL)     /* wholly below clip window */
            return -1;
        rc = 1;
    }
    if (vars->DESTY > YMAXCL)               /* wholly above clip window */
        return -1;
    if (vars->DESTY+dely > YMAXCL)          /* partially above clip window */
        rc = 1;

    return rc;
}


/*
 * do the actual clipping
 *
 * returns  0 OK
 *         -1 entirely clipped, nothing to output.  I believe this can
 *            only happen if rotation or scaling has been done
 */
static WORD do_clip(LOCALVARS *vars)
{
    WORD n;

    /*
     * if clipping not requested, exit
     */
    if (!CLIP)
        return 0;

    /*
     * clip x minimum if necessary
     */
    if (vars->DESTX < XMINCL)
    {
        n = vars->DESTX + vars->DELX - XMINCL;
        if (n <= 0)
            return -1;
        SOURCEX += vars->DELX - n;
        vars->DELX = n;
        vars->DESTX = XMINCL;
    }

    /*
     * clip x maximum if necessary
     */
    if (vars->DESTX > XMAXCL)
        return -1;
    n = vars->DESTX + vars->DELX - XMAXCL - 1;
    if (n > 0)
        vars->DELX -= n;

    /*
     * clip y minimum if necessary
     */
    if (vars->DESTY < YMINCL)
    {
        n = vars->DESTY + vars->DELY - YMINCL;
        if (n <= 0)
            return -1;
        SOURCEY += vars->DELY - n;
        vars->DELY = n;
        vars->DESTY = YMINCL;
    }

    /*
     * clip y maximum if necessary
     */
    if (vars->DESTY > YMAXCL)
        return -1;
    n = vars->DESTY + vars->DELY - YMAXCL - 1;
    if (n > 0)
        vars->DELY -= n;

    return 0;
}


/*
 * return a ULONG equal to the 3 high-order bytes pointed to by
 * the input pointer, concatenated with the low-order byte of
 * the input UWORD
 */
static ULONG merge_byte(UWORD *p, UWORD n)
{
    union {
        ULONG a;
        UBYTE b[4];
    } work;

    work.a = *(ULONG *)p;
    work.b[3] = n;

    return work.a;
}


/*
 * outline: perform text outlining
 *
 * in the following code, the top 18 bits of unsigned longs are used
 * to manage 18-bit values, consisting of the last bit of the previous
 * screen word, the 16 bits of the current screen word, and the first
 * bit of the next screen word.
 *
 * neighbours are numbered as follows:
 *              1 2 3
 *              7 X 8
 *              4 5 6
 */
void outline(LOCALVARS *vars)
{
    UWORD *currline, *nextline, *scratch;
    UWORD *save_next;
    UWORD curr, prev, tmp;
    WORD i, j, form_width;
    ULONG current_left, current_noshift, current_right;
    ULONG top_left, top_noshift, top_right;
    ULONG bottom_left, bottom_noshift, bottom_right;
    ULONG result;

    form_width = vars->s_next / sizeof(WORD);
    currline = (UWORD *)vars->sform + form_width;
    nextline = currline + form_width;

    /* process lines sequentially */
    for (i = vars->DELY; i > 0; i--)
    {
        save_next = nextline;
        prev = 0;
        curr = 0;
        bottom_left = (*(ULONG *)nextline) >> 1;

        /* process one word at a time */
        for (j = form_width, scratch = (UWORD *)vars->sform; j > 0; j--)
        {
            /* get the data from the current line */
            current_left = current_noshift = merge_byte(currline, curr);
            rorl(current_noshift, 1);
            current_right = current_noshift;
            rorl(current_right, 1);

            /*
             * get the data from the scratch buffer (the previous line)
             * and merge into result
             */
            top_right = merge_byte(scratch, prev);
            rorl(top_right, 1);
            top_left = top_noshift = top_right;
            top_left ^= current_left;           /* neighbours 1,2,3 */
            top_noshift ^= current_noshift;
            top_right ^= current_right;
            roll(top_noshift, 1);
            roll(top_right, 2);
            result = (top_left | top_noshift | top_right);

            /* get the data from the next line & merge into result */
            bottom_right = bottom_noshift = bottom_left;
            bottom_left ^= current_left;        /* neighbours 4,5,6 */
            bottom_noshift ^= current_noshift;
            bottom_right ^= current_right;
            roll(bottom_noshift, 1);
            roll(bottom_right, 2);
            result |= (bottom_left | bottom_noshift | bottom_right);

            /* finally, merge current line neighbours */
            current_left ^= current_noshift;    /* neighbours 7,8 */
            current_right ^= current_noshift;
            roll(current_right, 2);
            result |= (current_left | current_right);
            result >>= 16;                      /* move to lower 16 bits */

            prev = curr = *currline;
            prev = (prev ^ result) & result;
            *currline++ = prev;
            prev = *scratch;
            *scratch++ = curr;

            tmp = *nextline++;
            bottom_left = merge_byte(nextline, tmp);
            rorl(bottom_left, 1);
        }

        nextline = save_next;
        currline = nextline;

        if (i > 2)                              /* mustn't go past end */
            nextline += form_width;
    }
}


/*
 * copy the character into a buffer so that we can
 * outline/rotate/scale it
 */
static void pre_blit(LOCALVARS *vars)
{
    WORD weight, skew, size, n, tmp_style;
    WORD dest_width, dest_height;
    WORD *p;
    LONG offset;
    UBYTE *src, *dst;

    vars->height = vars->DELY;

    vars->tsdad = SOURCEX & 0x000f;     /* source dot address */
    offset = (SOURCEY+vars->DELY-1) * (LONG)vars->s_next + ((SOURCEX >> 3) & ~1);
    src = vars->sform + offset;         /* bottom of font char source */
    vars->s_next = -vars->s_next;       /* we draw from the bottom up */

    weight = WEIGHT;
    skew = LOFF + ROFF;

    dest_width = vars->DELX;
    if (vars->STYLE & F_THICKEN)
    {
        dest_width += weight;
        vars->smear = weight;
    }

    /*
     * handle outlining
     */
    vars->tddad = 0;
    dest_height = vars->DELY;
    if (vars->STYLE & F_OUTLINE)
    {
        dest_width += 3;        /* add 1 left & 2 right pixels */
        vars->tddad += 1;       /* and make leftmost column blank */
        vars->DELY += 2;        /* add 2 rows */
        dest_height += 3;       /* add 3 rows for buffer clear */
    }
    vars->width = dest_width;
    dest_width += skew;
    vars->DELX = dest_width;
    dest_width = ((dest_width >> 4) << 1) + 2;    /* in bytes */
    vars->d_next = -dest_width;
    size = dest_width * (dest_height - 1);
    vars->buffa = SCRPT2 - vars->buffa; /* switch buffers */
    dst = (UBYTE *)SCRTCHP + vars->buffa;
    vars->sform = dst;
    if (vars->STYLE & (F_OUTLINE|F_SKEW))
    {
        p = (WORD *)vars->sform;
        n = (size - vars->d_next) / 2;  /* add bottom line */
        while(n--)
            *p++ = 0;               /* clear buffer */
        if (vars->STYLE & F_OUTLINE)
        {
            vars->width -= 3;
            vars->DELX -= 1;
            size += vars->d_next;
        }
    }

    dst += size;                     /* start at the bottom */
    vars->WRT_MODE = 0;
    vars->forecol = 1;
    vars->ambient = 0;
    vars->nbrplane = 1;             /* 1 plane for this blit */
    vars->nextwrd = 2;
    tmp_style = vars->STYLE;        /* save temporarily */
    vars->STYLE &= (F_SKEW|F_THICKEN);  /* only thicken, skew */

    normal_blit(vars+1, src, dst);  /* call assembler helper function */

    vars->STYLE = tmp_style;        /* restore */
    vars->WRT_MODE = WRT_MODE;
    vars->s_next = -vars->d_next;   /* reset the source to the buffer */

    if (vars->STYLE & F_OUTLINE)
    {
        outline(vars);
        vars->sform += vars->s_next;
    }

    SOURCEX = 0;
    SOURCEY = 0;
    vars->STYLE &= ~(F_SKEW|F_THICKEN); /* cancel effects */
}


/*
 * rotate: perform text rotation
 */
void rotate(LOCALVARS *vars)
{
    UBYTE *src, *dst;
    WORD form_width, i, j, k, tmp;
    UWORD in, out, srcbit, dstbit;
    UWORD *p, *q;

    vars->tsdad = SOURCEX & 0x000f;
    src = vars->sform + ((SOURCEX >> 4) << 1);
    vars->buffa = SCRPT2 - vars->buffa;     /* switch buffers */
    dst = (UBYTE *)SCRTCHP + vars->buffa;

    vars->width = vars->DELX;
    vars->height = vars->DELY;

    /*
     * first, handle the simplest case: inverted text (180 rotation)
     */
    if (CHUP == 1800)
    {
        form_width = ((vars->DELX+vars->tsdad-1) >> 4) + 1; /* in words */
        vars->d_next = form_width * sizeof(WORD);
        q = (UWORD *)(dst + vars->d_next * vars->height);
        for (i = vars->height; i > 0; i--)
        {
            for (j = form_width, p = (UWORD *)src; j > 0; j--)
            {
                in = *p++;
                out = reverse_nybble[in&0x000f];    /* reverse 4 bits at a time */
                for (k = 3; k > 0; k--)
                {
                    out <<= 4;
                    in >>= 4;
                    out |= reverse_nybble[in&0x000f];
                }
                *--q = out;
            }
            src += vars->s_next;
        }
        vars->s_next = vars->d_next;
        vars->sform = (UBYTE *)SCRTCHP + vars->buffa;
        SOURCEX = -(SOURCEX + vars->DELX) & 0x000f;
        SOURCEY = 0;
        return;
    }

    /*
     * handle remaining cases (90 and 270 rotation)
     */
    vars->d_next = ((vars->DELY >> 4) << 1) + 2;

    if (CHUP == 900)
    {
        dst += (vars->DELX - 1) * vars->d_next;
        vars->d_next = -vars->d_next;
    }
    else        /* 2700 */
    {
        src += (SOURCEY + vars->height - 1) * vars->s_next;
        vars->s_next = -vars->s_next;
    }

    srcbit = 0x8000 >> vars->tsdad;
    dstbit = 0x8000;
    out = 0;
    p = (UWORD *)src;
    q = (UWORD *)dst;
    for (i = vars->width; i > 0; i--)
    {
        for (j = vars->height; j > 0; j--)
        {
            if (*p & srcbit)
                out |= dstbit;
            dstbit >>= 1;
            if (!dstbit)
            {
                dstbit = 0x8000;
                *q++ = out;
                out = 0;
            }
            p = (UWORD *)((UBYTE *)p + vars->s_next);
        }

        dstbit = 0x8000;
        *q = out;
        out = 0;
        dst += vars->d_next;
        q = (UWORD *)dst;

        srcbit >>= 1;
        if (!srcbit)
        {
            srcbit = 0x8000;
            src += sizeof(WORD);
        }

        p = (UWORD *)src;
    }

    vars->height = vars->DELX;  /* swap width & height */
    vars->width = vars->DELY;
    vars->DELX = vars->width;
    vars->DELY = vars->height;
    tmp = vars->tmp_delx;
    vars->tmp_delx = vars->tmp_dely;
    vars->tmp_dely = tmp;
    vars->swap_tmps = 1;

    vars->s_next = (CHUP == 900) ? -vars->d_next : vars->d_next;
    vars->sform = (UBYTE*)SCRTCHP + vars->buffa;
    SOURCEX = 0;
    SOURCEY = 0;
}


/*
 * inline function to clarify horizontal scaling code
 */
static __inline__ UWORD *shift_and_update(UWORD *dst, UWORD *dstbit, UWORD *out)
{
    *dstbit >>= 1;
    if (!*dstbit)           /* end of word ? */
    {
        *dstbit = 0x8000;   /* reset test bit */
        *dst++ = *out;      /* output accumulated word */
        *out = 0;           /* & reset it */
    }

    return dst;
}


/*
 * scaleup: increase width of character (SCALDIR is 1)
 */
static void scaleup(LOCALVARS *vars, UWORD *src, UWORD *dst)
{
    UWORD srcbit, dstbit;
    UWORD accum, in, out;
    WORD i;

    srcbit = 0x8000 >> vars->tsdad;
    dstbit = 0x8000;

    out = 0;
    accum = XDDA;
    in = *src++;        /* prime the source word */

    for (i = vars->width; i > 0; i--)
    {
        if (in & srcbit)            /* handle bit set in source */
        {
            accum += DDAINC;
            if (accum < DDAINC)
            {
                out |= dstbit;
                dst = shift_and_update(dst, &dstbit, &out);
            }
            out |= dstbit;
            dst = shift_and_update(dst, &dstbit, &out);
        }
        else                        /* handle bit clear in source */
        {
            accum += DDAINC;
            if (accum < DDAINC)
            {
                dst = shift_and_update(dst, &dstbit, &out);
            }
            dst = shift_and_update(dst, &dstbit, &out);
        }

        srcbit >>= 1;
        if (!srcbit)
        {
            srcbit = 0x8000;
            in = *src++;
        }
    }

    *dst = out;
}


/*
 * scaledown: decrease width of character (SCALDIR is 0)
 */
static void scaledown(LOCALVARS *vars, UWORD *src, UWORD *dst)
{
    UWORD srcbit, dstbit;
    UWORD accum, in, out;
    WORD i;

    srcbit = 0x8000 >> vars->tsdad;
    dstbit = 0x8000;

    out = 0;
    accum = XDDA;
    in = *src++;        /* prime the source word */

    for (i = vars->width; i > 0; i--)
    {
        if (in & srcbit)            /* handle bit set in source */
        {
            accum += DDAINC;
            if (accum < DDAINC)
            {
                out |= dstbit;
                dst = shift_and_update(dst, &dstbit, &out);
            }
        }
        else                        /* handle bit clear in source */
        {
            dst = shift_and_update(dst, &dstbit, &out);
        }

        srcbit >>= 1;
        if (!srcbit)
        {
            srcbit = 0x8000;
            in = *src++;
        }
    }

    *dst = out;
}


/*
 * scale: perform text scaling
 */
void scale(LOCALVARS *vars)
{
    UBYTE *src, *dst;
    WORD i, delx;
    UWORD accum;

    vars->tsdad = SOURCEX & 0x000f;
    src = vars->sform + ((SOURCEX >> 4) << 1) + (SOURCEY * vars->s_next);

    vars->buffa = SCRPT2 - vars->buffa;     /* switch buffers */
    dst = (UBYTE *)SCRTCHP + vars->buffa;

    vars->width = vars->DELX;
    vars->height = vars->DELY;

    vars->d_next = ((vars->width >> 3) << 1) + 2;

    /*
     * first, scale the character
     */
    accum = 0x7fff;
    if (SCALDIR)        /* scale up */
    {
        for (i = vars->height; i > 0; i--)
        {
            accum += DDAINC;
            if (accum < DDAINC)
            {
                scaleup(vars, (UWORD *)src, (UWORD *)dst);
                dst += vars->d_next;
            }
            scaleup(vars, (UWORD *)src, (UWORD *)dst);
            dst += vars->d_next;
            src += vars->s_next;
        }
    }
    else                /* scale down */
    {
        for (i = vars->height; i > 0; i--)
        {
            accum += DDAINC;
            if (accum < DDAINC)
            {
                scaledown(vars, (UWORD *)src, (UWORD *)dst);
                dst += vars->d_next;
            }
            src += vars->s_next;
        }
    }

    /*
     * then, adjust the character spacing
     */
    accum = XDDA;
    delx = SCALDIR ? vars->DELX : 0;
    for (i = vars->DELX; i > 0; i--)
    {
        accum += DDAINC;
        if (accum < DDAINC)
            delx++;
    }
    XDDA = accum;

    vars->DELX = delx;
    vars->DELY = vars->tmp_dely;
    vars->s_next = vars->d_next;
    vars->sform = (UBYTE *)SCRTCHP + vars->buffa;
    SOURCEX = 0;
    SOURCEY = 0;
}


#if CONF_WITH_VDI_FASTDRAW && CONF_WITH_VDI_16BIT
/*
 * 16-bit expands, two pixels per store
 *
 * a pair of source bits indexes a 4-entry table of pixel pairs, so one
 * long store (or one read/AND/XOR/write for the modes that leave some
 * pixels alone) draws two pixels.  the tables are built once per call.
 */

/*
 * build the tables for one mode
 *  replace: source 1 -> fg, source 0 -> bg
 *  trans:   source 1 -> fg, source 0 left alone
 *  xor:     source 1 -> complement, source 0 left alone
 *  erase:   source 0 -> bg, source 1 left alone (each caller passes the
 *           colour its own erase rule wants as bg)
 */
const struct expand16 *expand_tab16(WORD kind, UWORD fg, UWORD bg)
{
    static struct expand16 tab;     /* in BSS: kind is stored +1, 0 means empty */
    struct expand16 *e = &tab;
    UWORD sa, sx, ca, cx, ha, hx, la, lx;
    WORD k;

    /* text calls this once per glyph: keep the last table */
    if (e->kind == kind + 1 && e->fg == fg && e->bg == bg)
        return e;

    switch(kind) {
    case EXP16_TRANS:
        sa = 0;      sx = fg;     ca = 0xffff; cx = 0;
        e->masked = 1;
        e->mod = 3;
        e->inv = 0;
        break;
    case EXP16_XOR:
        sa = 0xffff; sx = 0xffff; ca = 0xffff; cx = 0;
        e->masked = 1;
        e->mod = 3;
        e->inv = 0;
        break;
    case EXP16_ERASE:
        sa = 0xffff; sx = 0;      ca = 0;      cx = bg;
        e->masked = 1;
        e->mod = 0;         /* the pixels changed are those of the clear source bits */
        e->inv = 0xffff;
        break;
    default:        /* EXP16_REPLACE */
        sa = 0;      sx = fg;     ca = 0;      cx = bg;
        e->masked = 0;
        e->mod = 3;
        e->inv = 0;
        break;
    }

    for (k = 0; k < 4; k++)
    {
        if (k & 2)
        {
            ha = sa;
            hx = sx;
        }
        else
        {
            ha = ca;
            hx = cx;
        }
        if (k & 1)
        {
            la = sa;
            lx = sx;
        }
        else
        {
            la = ca;
            lx = cx;
        }
        e->a[k] = ((ULONG)ha << 16) | la;
        e->x[k] = ((ULONG)hx << 16) | lx;
    }
    e->kind = kind + 1;
    e->fg = fg;
    e->bg = bg;
    return e;
}

/*
 * 'rows' rows of 'width' pixels from the monochrome bits at 'src', whose
 * first pixel is bit 'bit' (0-15, from the msb) of the first source word;
 * the next row is 'sstep' bytes from the source, 'dstep' bytes from the
 * destination.  the source words read are the ones the pixels need, so a
 * caller's row stride can assume 1 + ((bit + width - 1) >> 4) words.
 * a pair of pixels is one long store; what does not change from row to row
 * (alignment, plain or masked) is decided once.  the alignment is that of
 * the first row: with a row pitch that is a multiple of 4 bytes (the screen
 * in every 16-bit mode) every row's long stores are aligned.  an off-screen
 * bitmap in a program's memory can have a pitch of 2 mod 4 bytes; every
 * other row's long stores are then misaligned, which only costs time on the
 * 68020 and later, the only processors with 16-bit screens.
 */
#define XP(t,off)   (*(const ULONG *)((const UBYTE *)(t) + (off)))
#define PX(w,j)     (((w) >> (12 - 2*(j))) & 12)

/*
 * store px into the pixels whose bit is set in w (msb first) at d, a pair
 * of bits at a time: where most pixels are drawn (erase) that costs less
 * than testing each one
 */
#define STORE_BITS16(d, w, px)                          \
    do {                                                \
        UWORD *dp_ = (d);                               \
        UWORD wp_ = (w);                                \
        for ( ; wp_; wp_ <<= 2, dp_ += 2)               \
        {                                               \
            if (wp_ & 0xc000)                           \
            {                                           \
                if (wp_ & 0x8000)                       \
                    dp_[0] = (px);                      \
                if (wp_ & 0x4000)                       \
                    dp_[1] = (px);                      \
            }                                           \
        }                                               \
    } while(0)

static void expand_small16_plain(UWORD *dst, const UWORD *src, WORD bit, WORD width, WORD rows,
                    LONG sstep, LONG dstep, const struct expand16 *e)
{
    const ULONG *xt = e->x;
    UWORD x0 = (UWORD)xt[0], x1 = (UWORD)xt[3];
    UWORD w;
    WORD lead = ((ULONG)dst & 2) ? 1 : 0;
    WORD pairs = (width - lead) >> 1;
    WORD trail = (width - lead) & 1;
    ULONG *l;
    UWORD *d;

    for ( ; rows > 0; rows--)
    {
        w = *src << bit;
        d = dst;
        if (lead)
        {
            *d = (w & 0x8000) ? x1 : x0;
            d++;
            w <<= 1;
        }
        l = (ULONG *)d;
        switch(pairs) {
        case 8: l[7] = XP(xt, (w << 2) & 12);
        /* fall through */
        case 7: l[6] = XP(xt, PX(w,6));
        /* fall through */
        case 6: l[5] = XP(xt, PX(w,5));
        /* fall through */
        case 5: l[4] = XP(xt, PX(w,4));
        /* fall through */
        case 4: l[3] = XP(xt, PX(w,3));
        /* fall through */
        case 3: l[2] = XP(xt, PX(w,2));
        /* fall through */
        case 2: l[1] = XP(xt, PX(w,1));
        /* fall through */
        case 1: l[0] = XP(xt, PX(w,0));
        /* fall through */
        default:
            break;
        }
        if (trail)
        {
            d += pairs << 1;
            w <<= (pairs << 1);
            *d = (w & 0x8000) ? x1 : x0;
        }
        src = (const UWORD *)((const UBYTE *)src + sstep);
        dst = (UWORD *)((UBYTE *)dst + dstep);
    }
}

static void expand_small16_masked(UWORD *dst, const UWORD *src, WORD bit, WORD width, WORD rows,
                           LONG sstep, LONG dstep, const struct expand16 *e)
{
    UWORD px = (UWORD)e->x[e->mod];
    UWORD keep = (UWORD)(0xffffU << (16 - width));
    UWORD inv = e->inv;
    UWORD w;
    UWORD *d;
    WORD n;

    /*
     * only the pixels the mode changes are visited, by testing bits, not
     * through the pair table: where branches are cheap and stores are not
     * (QEMU), that is the faster way
     */
    for ( ; rows > 0; rows--)
    {
        w = (*src << bit) ^ inv;
        d = dst;
        if (e->a[e->mod])           /* xor */
        {
            for (w &= keep; w; w <<= 1, d++)
            {
                if (w & 0x8000)
                    *d = ~*d;
            }
        }
        else if (inv)               /* erase: most pixels are drawn */
        {
            for (n = width; n > 0; n--, w <<= 1, d++)
            {
                if (w & 0x8000)
                    *d = px;
            }
        }
        else                        /* transparent: the set pixels */
        {
            for (w &= keep; w; w <<= 1, d++)
            {
                if (w & 0x8000)
                    *d = px;
            }
        }
        src = (const UWORD *)((const UBYTE *)src + sstep);
        dst = (UWORD *)((UBYTE *)dst + dstep);
    }
}

static void expand_rows16_plain(UWORD *dst, const UWORD *src, WORD bit, LONG width, WORD rows,
                    LONG sstep, LONG dstep, const struct expand16 *e)
{
    const ULONG *xt = e->x;
    UWORD x0 = (UWORD)xt[0], x1 = (UWORD)xt[3];
    const UWORD *s;
    UWORD w;
    WORD lead = ((ULONG)dst & 2) ? 1 : 0;
    WORD sb, n, pairs;
    LONG left;
    ULONG *l;
    UWORD *d;

    for ( ; rows > 0; rows--)
    {
        d = dst;
        s = src;
        sb = bit;
        left = width;
        if (lead)               /* long stores need an even word address */
        {
            w = *s << sb;
            *d = (w & 0x8000) ? x1 : x0;
            d++;
            left--;
            if (++sb > 15)
            {
                sb = 0;
                s++;
            }
        }
        l = (ULONG *)d;
        while (left > 0)        /* up to 16 pixels, i.e. one source word, a time */
        {
            n = (left < 16) ? (WORD)left : 16;
            w = *s << sb;
            if (sb && n > 16 - sb)
                w |= s[1] >> (16 - sb);
            s++;
            pairs = n >> 1;
            switch(pairs) {
            case 8: l[7] = XP(xt, (w << 2) & 12);
            /* fall through */
            case 7: l[6] = XP(xt, PX(w,6));
            /* fall through */
            case 6: l[5] = XP(xt, PX(w,5));
            /* fall through */
            case 5: l[4] = XP(xt, PX(w,4));
            /* fall through */
            case 4: l[3] = XP(xt, PX(w,3));
            /* fall through */
            case 3: l[2] = XP(xt, PX(w,2));
            /* fall through */
            case 2: l[1] = XP(xt, PX(w,1));
            /* fall through */
            case 1: l[0] = XP(xt, PX(w,0));
            /* fall through */
            default:
                break;
            }
            l += pairs;
            if (n & 1)          /* the last, odd pixel */
            {
                d = (UWORD *)l;
                *d = ((UWORD)(w << (pairs << 1)) & 0x8000) ? x1 : x0;
            }
            left -= n;
        }
        src = (const UWORD *)((const UBYTE *)src + sstep);
        dst = (UWORD *)((UBYTE *)dst + dstep);
    }
}

static void expand_rows16_masked(UWORD *dst, const UWORD *src, WORD bit, LONG width, WORD rows,
                    LONG sstep, LONG dstep, const struct expand16 *e)
{
    UWORD pa = (UWORD)e->a[e->mod], px = (UWORD)e->x[e->mod];
    UWORD inv = e->inv;
    const UWORD *s;
    UWORD w;
    WORD n;
    LONG left;
    UWORD *d, *dd;

    for ( ; rows > 0; rows--)
    {
        d = dst;
        s = src;
        left = width;
        while (left > 0)        /* up to 16 pixels, i.e. one source word, a time */
        {
            n = (left < 16) ? (WORD)left : 16;
            w = *s << bit;
            if (bit && n > 16 - bit)
                w |= s[1] >> (16 - bit);
            s++;
            w = (w ^ inv) & (UWORD)(0xffffU << (16 - n));
            dd = d;
            if (pa)
            {
                for ( ; w; w <<= 1, dd++)
                {
                    if (w & 0x8000)
                        *dd = ~*dd;
                }
            }
            else
                STORE_BITS16(dd, w, px);
            d += n;
            left -= n;
        }
        src = (const UWORD *)((const UBYTE *)src + sstep);
        dst = (UWORD *)((UBYTE *)dst + dstep);
    }
}

void expand_rows16(UWORD *dst, const UWORD *src, WORD bit, LONG width, WORD rows,
                   LONG sstep, LONG dstep, const struct expand16 *e)
{
    if (width <= 0)
        return;
    if (bit + width <= 16)      /* all in one source word: the usual glyph */
    {
        if (e->masked)
            expand_small16_masked(dst, src, bit, (WORD)width, rows, sstep, dstep, e);
        else
            expand_small16_plain(dst, src, bit, (WORD)width, rows, sstep, dstep, e);
    }
    else if (e->masked)
        expand_rows16_masked(dst, src, bit, width, rows, sstep, dstep, e);
    else
        expand_rows16_plain(dst, src, bit, width, rows, sstep, dstep, e);
}
#endif


#if CONF_WITH_VDI_TEXT_SPEEDUP
#if CONF_WITH_VDI_16BIT
/*
 * output a character string directly to the 16-bit screen
 *
 * see direct_screen_blit() for details of usage
 */
static void direct_screen_blit16(WORD count, WORD *str)
{
    WORD fgcol, bgcol, height, mode, n;
    WORD src_width, dst_width;
    UBYTE *src, *p;
    UWORD *dst, *q, *palette;
    UBYTE mask;
    UWORD *save_dst;
#if CONF_WITH_VDI_FASTDRAW
    const struct expand16 *e;
    WORD kind;
#endif

    height = DELY;
    mode = WRT_MODE;
    src_width = FWIDTH;
    dst_width = v_lin_wr / sizeof(UWORD);

    palette = CUR_WORK->ext->palette;
    fgcol = palette[TEXTFG];
#if CONF_WITH_VDI_NVDI_EXT
    bgcol = palette[vdi_bg_text];
#else
    bgcol = palette[0];
#endif

    dst = get_start_addr16(DESTX, DESTY);

#if CONF_WITH_VDI_FASTDRAW
    /*
     * an 8-pixel row is at most 4 long stores, built from the 8 source bits.
     * erase keeps the loops below: under QEMU they are faster than any way
     * of visiting the many pixels it draws
     */
    if (mode != WM_ERASE)
    {
        switch(mode) {
        default:        /* WM_REPLACE */
            kind = EXP16_REPLACE;
            break;
        case WM_TRANS:
            kind = EXP16_TRANS;
            break;
        case WM_XOR:
            kind = EXP16_XOR;
            break;
        }
        e = expand_tab16(kind, fgcol, bgcol);

        for ( ; count > 0; count--)
        {
            src = (UBYTE *)FBASE + *str++;
            if ((ULONG)dst & 2)         /* not a long word address: generic path */
            {
                for (n = height, p = src, q = dst; n > 0; n--)
                {
                    UWORD row = (UWORD)*p << 8;

                    expand_rows16(q, &row, 0, 8, 1, 0, 0, e);
                    p += src_width;
                    q += dst_width;
                }
            }
            else if (e->masked)
            {
                UWORD pa = (UWORD)e->a[e->mod], px = (UWORD)e->x[e->mod];

                /* only the pixels the mode changes, tested bit by bit (see expand_small16_masked) */
                for (n = height, p = src, q = dst; n > 0; n--)
                {
                    UWORD w = (UWORD)((*p ^ e->inv) & 0xff) << 8;
                    UWORD *d;

                    d = q;
                    if (pa)
                    {
                        for ( ; w; w <<= 1, d++)
                        {
                            if (w & 0x8000)
                                *d = ~*d;
                        }
                    }
                    else
                        STORE_BITS16(d, w, px);
                    p += src_width;
                    q += dst_width;
                }
            }
            else
            {
                for (n = height, p = src, q = dst; n > 0; n--)
                {
                    ULONG *l = (ULONG *)q;
                    UWORD b = *p;

                    l[0] = e->x[b >> 6];
                    l[1] = e->x[(b >> 4) & 3];
                    l[2] = e->x[(b >> 2) & 3];
                    l[3] = e->x[b & 3];
                    p += src_width;
                    q += dst_width;
                }
            }
            dst += 8;
        }
        return;
    }
#endif
    for ( ; count > 0; count--)
    {
        src = (UBYTE *)FBASE + *str++;
        save_dst = dst;

        switch(mode) {
        default:    /* WM_REPLACE */
            for (n = height, p = src; n > 0; n--)
            {
                for (mask = 0x80, q = dst; mask; mask >>= 1)
                {
                    *q++ = (*p & mask) ? fgcol : bgcol;
                }
                p += src_width;
                dst += dst_width;
            }
            break;
        case WM_TRANS:
            for (n = height, p = src; n > 0; n--)
            {
                for (mask = 0x80, q = dst; mask; mask >>= 1)
                {
                    if (*p & mask)
                        *q = fgcol;
                    q++;
                }
                p += src_width;
                dst += dst_width;
            }
            break;
        case WM_XOR:
            for (n = height, p = src; n > 0; n--)
            {
                for (mask = 0x80, q = dst; mask; mask >>= 1)
                {
                    if (*p & mask)
                        *q = ~*q;
                    q++;
                }
                p += src_width;
                dst += dst_width;
            }
            break;
        case WM_ERASE:
            for (n = height, p = src; n > 0; n--)
            {
                for (mask = 0x80, q = dst; mask; mask >>= 1)
                {
                    /*
                     * note: here we differ from TOS 4.04 which seems to
                     * behave as though the assignment below was "*q = bgcol;".
                     * the TOS4.04 behaviour is a bug IMO.
                     */
                    if (!(*p & mask))
                        *q = fgcol;
                    q++;
                }
                p += src_width;
                dst += dst_width;
            }
            break;
        }

        dst = save_dst + 8;
    }
}
#endif


/*
 * output a character string directly to the screen
 *
 * this is used for the special (but common) case of a string with no
 * special effects, no rotation, no justification, left-alignment,
 * byte-aligned output, using a monospaced font with a cell width of 8
 *
 * note: like Atari TOS, we assume that the font contains the full
 * character set, i.e. first_ade==0, last_ade==255
 */
void direct_screen_blit(WORD count, WORD *str)
{
    WORD forecol, height, mode, n, planes;
    WORD src_width, dst_width;
    UBYTE *src, *dst, *save_dst;

    height = DELY;
    mode = WRT_MODE;
    src_width = FWIDTH;
    dst_width = v_lin_wr;

#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE)
    {
        direct_screen_blit16(count, str);
        return;
    }
#endif

    dst = (UBYTE *)get_start_addr(DESTX, DESTY);
    if (DESTX & 0x0008)
        dst++;

    for ( ; count > 0; count--)
    {
        src = (UBYTE *)FBASE + *str++;
        save_dst = dst;
        forecol = TEXTFG;
        for (planes = v_planes; planes > 0; planes--)
        {
            UBYTE *p, *q;

            switch(mode) {
            default:    /* WM_REPLACE */
                if (forecol & 1)
                {
                    for (n = height, p = src, q = dst; n > 0; n--)
                    {
                        *q = *p;
                        p += src_width;
                        q += dst_width;
                    }
                }
                else
                {
                    for (n = height, q = dst; n > 0; n--)
                    {
                        *q = 0;
                        q += dst_width;
                    }
                }
                break;
            case WM_TRANS:
                if (forecol & 1)
                {
                    for (n = height, p = src, q = dst; n > 0; n--)
                    {
                        *q |= *p;
                        p += src_width;
                        q += dst_width;
                    }
                }
                else
                {
                    for (n = height, p = src, q = dst; n > 0; n--)
                    {
                        *q &= ~*p;
                        p += src_width;
                        q += dst_width;
                    }
                }
                break;
            case WM_XOR:
                for (n = height, p = src, q = dst; n > 0; n--)
                {
                    *q ^= *p;
                    p += src_width;
                    q += dst_width;
                }
                break;
            case WM_ERASE:
                if (forecol & 1)
                {
                    for (n = height, p = src, q = dst; n > 0; n--)
                    {
                        *q |= ~*p;
                        p += src_width;
                        q += dst_width;
                    }
                }
                else
                {
                    for (n = height, p = src, q = dst; n > 0; n--)
                    {
                        *q &= *p;
                        p += src_width;
                        q += dst_width;
                    }
                }
                break;
            }
            dst += sizeof(WORD);    /* next plane */
            forecol >>= 1;
        }
        dst = save_dst + 1;
        if (!IS_ODD_POINTER(dst))   /* must go to next screen word */
        {
            dst += (v_planes-1)*sizeof(WORD);
        }
    }
}
#endif


#if CONF_WITH_VDI_16BIT
/*
 * output a glyph to the 16-bit screen
 */
static void screen_blit16(LOCALVARS *vars)
{
    UWORD *palette;
    UBYTE *src, *dst;
    WORD fgcol, bgcol, h, skew, skew_start;
    UWORD skew_mask;
#if CONF_WITH_VDI_FASTDRAW
    const struct expand16 *e;
    WORD kind, bit;
#else
    UWORD *p, *q;
    WORD w;
    UWORD src_mask, mask;
#endif

    /*
     * set skew-related values
     *
     * NOTE: we can't test for skewed text using vars->STYLE, since
     * pre_blit() clears F_SKEW and F_THICKEN after it has processed them.
     */
    skew = LOFF + ROFF;
    skew_mask = (UWORD)vars->skew_msk;
    skew_start = vars->height;

    /*
     * the following adjustments are for skewed+outlined text, and make
     * the output almost the same as produced by TOS4.
     *
     * 1. since the source of skewed and/or outlined text must be an
     *    intermediate buffer, SOURCEX *must* be 0, and we force that.
     *    NOTE: in versions of TOS prior to TOS4 (& in TOS4 non-TC
     *    resolutions), this adjustment is not made.  As a result, text
     *    output is typically clipped.
     *
     * 2. a negative value for the nominal destination position is OK,
     *    because outlining has adjusted the starting position of characters
     *    leftwards.  however, such values are prohibited by do_clip(),
     *    which adjusts var->DESTX.  we adjust it back here ...
     *    NOTE: this situation can only happen at the beginning of a
     *    screen line.
     *
     * 3. for bigger fonts, skewing must not start at the bottom of the
     *    buffer, otherwise parts of the outline are clipped too agressively.
     *    at the moment, this fix is a bit of a kludge, though it works well
     *    enough.
     */
    if (skew && (vars->STYLE&F_OUTLINE))
    {
        if (SOURCEX)
        {
            KDEBUG(("SOURCEX (was %d) forced to zero for intermediate buffer\n",SOURCEX));
            SOURCEX = 0;
            vars->tsdad = 0;    /* this was set from SOURCEX in screen_blit() */
        }

        if (DESTX < 0)
        {
            KDEBUG(("vars->DESTX (was %d) set to DESTX (%d)\n",vars->DESTX,DESTX));
            vars->DESTX = DESTX;
        }
        if (vars->height > 8)       /* not a 6-point font */
            skew_start -= OUTLINE_THICKNESS;
    }

    /*
     * set up source stuff
     */
    src = vars->sform;
#if CONF_WITH_VDI_FASTDRAW
    bit = vars->tsdad;
#else
    src_mask = 0x8000 >> vars->tsdad;
#endif

    /*
     * set up destination stuff
     */
    vars->dform = v_bas_ad;
    vars->dform += vars->DESTX * sizeof(WORD);      /* add x coordinate part of addr */
    vars->dform += (UWORD)(vars->DESTY+vars->DELY-1) * (ULONG)v_lin_wr; /* add y coordinate part of addr */
    vars->d_next = -v_lin_wr;
    dst = vars->dform;

    /*
     * set up colours
     */
    palette = CUR_WORK->ext->palette;
    fgcol = palette[vars->forecol];
#if CONF_WITH_VDI_NVDI_EXT
    bgcol = palette[vdi_bg_text];
#else
    bgcol = palette[0];
#endif

#if CONF_WITH_VDI_FASTDRAW
    /*
     * when called via lineA, modes 4-19 (corresponding to BitBlt modes 0-15)
     * are theoretically possible.  however, at this time we do not support them.
     */
    switch(vars->WRT_MODE) {
    default:    /* WM_REPLACE */
        kind = EXP16_REPLACE;
        break;
    case WM_TRANS:
        kind = EXP16_TRANS;
        break;
    case WM_XOR:
        kind = EXP16_XOR;
        break;
    case WM_ERASE:
        kind = EXP16_ERASE;
        bgcol = fgcol;      /* behaviour differs from TOS 4.04 - see direct_screen_blit16() */
        break;
    }
    e = expand_tab16(kind, fgcol, bgcol);

    if (!skew && vars->width > 0 && bit + vars->width <= 16)  /* a glyph within one source word */
    {
        if (e->masked)
            expand_small16_masked((UWORD *)dst, (const UWORD *)src, bit, vars->width, vars->height,
                                  vars->s_next, vars->d_next, e);
        else
            expand_small16_plain((UWORD *)dst, (const UWORD *)src, bit, vars->width, vars->height,
                                 vars->s_next, vars->d_next, e);
        return;
    }

    if (!skew)
    {
        expand_rows16((UWORD *)dst, (const UWORD *)src, bit, vars->width, vars->height,
                      vars->s_next, vars->d_next, e);
        return;
    }

    for (h = vars->height; h > 0; h--, src += vars->s_next, dst += vars->d_next)
    {
        expand_rows16((UWORD *)dst, (const UWORD *)src, bit, vars->width, 1, 0, 0, e);
        /*
         * special handling for skewed text: since the character cells
         * are effectively slanted, we must shift the starting position
         * of a cell rightwards as we go up the character.
         */
        if (skew && (h <= skew_start))  /* OK to shift box for skewed text? */
        {
            rolw1(skew_mask);
            if (skew_mask & 0x8000)
            {
                if (++bit > 15)
                {
                    bit = 0;
                    src++;
                }
                dst += sizeof(UWORD);
            }
        }
    }
#else
    switch(vars->WRT_MODE) {
    /*
     * when called via lineA, modes 4-19 (corresponding to BitBlt modes 0-15)
     * are theoretically possible.  however, at this time we do not support them.
     */
    default:    /* WM_REPLACE */
        for (h = vars->height; h > 0; h--, src += vars->s_next, dst += vars->d_next)
        {
            p = (UWORD *)src;
            q = (UWORD *)dst;
            for (w = vars->width, mask = src_mask; w > 0; w--)
            {
                *q++ = (*p & mask) ? fgcol : bgcol;
                rorw1(mask);
                if (mask == 0x8000)
                    p++;
            }
            /*
             * special handling for skewed text: since the character cells
             * are effectively slanted, we must shift the starting position
             * of a cell rightwards as we go up the character.
             */
            if (skew && (h <= skew_start))  /* OK to shift box for skewed text? */
            {
                rolw1(skew_mask);
                if (skew_mask & 0x8000)
                {
                    rorw1(src_mask);
                    if (src_mask == 0x8000)
                        src++;
                    dst += sizeof(UWORD);
                }
            }
        }
        break;
    case WM_TRANS:
        for (h = vars->height; h > 0; h--, src += vars->s_next, dst += vars->d_next)
        {
            p = (UWORD *)src;
            q = (UWORD *)dst;
            for (w = vars->width, mask = src_mask; w > 0; w--)
            {
                if (*p & mask)
                    *q = fgcol;
                q++;
                rorw1(mask);
                if (mask == 0x8000)
                    p++;
            }
            /*
             * see comments for WM_REPLACE (above) for an explanation of
             * the following
             */
            if (skew && (h <= skew_start))  /* OK to shift box for skewed text? */
            {
                rolw1(skew_mask);
                if (skew_mask & 0x8000)
                {
                    rorw1(src_mask);
                    if (src_mask == 0x8000)
                        src++;
                    dst += sizeof(UWORD);
                }
            }
        }
        break;
    case WM_XOR:
        for (h = vars->height; h > 0; h--, src += vars->s_next, dst += vars->d_next)
        {
            p = (UWORD *)src;
            q = (UWORD *)dst;
            for (w = vars->width, mask = src_mask; w > 0; w--)
            {
                if (*p & mask)
                    *q = ~*q;
                q++;
                rorw1(mask);
                if (mask == 0x8000)
                    p++;
            }
            /*
             * see comments for WM_REPLACE (above) for an explanation of
             * the following
             */
            if (skew && (h <= skew_start))  /* OK to shift box for skewed text? */
            {
                rolw1(skew_mask);
                if (skew_mask & 0x8000)
                {
                    rorw1(src_mask);
                    if (src_mask == 0x8000)
                        src++;
                    dst += sizeof(UWORD);
                }
            }
        }
        break;
    case WM_ERASE:
        for (h = vars->height; h > 0; h--, src += vars->s_next, dst += vars->d_next)
        {
            p = (UWORD *)src;
            q = (UWORD *)dst;
            for (w = vars->width, mask = src_mask; w > 0; w--)
            {
                /*
                 * behaviour here differs from TOS 4.04 - for further info,
                 * see the comments in direct_screen_blit16()
                 */
                if (!(*p & mask))
                    *q = fgcol;
                q++;
                rorw1(mask);
                if (mask == 0x8000)
                    p++;
            }
            /*
             * see comments for WM_REPLACE (above) for an explanation of
             * the following
             */
            if (skew && (h <= skew_start))  /* OK to shift box for skewed text? */
            {
                rolw1(skew_mask);
                if (skew_mask & 0x8000)
                {
                    rorw1(src_mask);
                    if (src_mask == 0x8000)
                        src++;
                    dst += sizeof(UWORD);
                }
            }
        }
        break;
    }
#endif
}
#endif


/*
 * output a glyph to the screen
 *
 * this is called each time a character is output to the screen, unless
 * CONF_WITH_VDI_TEXT_SPEEDUP is configured, and the string to be output
 * has no special features, in which case direct_screen_blit() is used
 * to output a whole string at once.
 */
static void screen_blit(LOCALVARS *vars)
{
    LONG offset;

    vars->forecol = TEXTFG;
    vars->ambient = 0;          /* logically TEXTBG, but that isn't set up by the VDI */
    vars->nbrplane = v_planes;
    vars->nextwrd = vars->nbrplane * sizeof(WORD);
    vars->height = vars->DELY;
    vars->width = vars->DELX;

    /*
     * calculate the starting address for the character to be copied
     */
    vars->tsdad = SOURCEX & 0x000f; /* source dot address */
    offset = (SOURCEY+vars->DELY-1) * (LONG)vars->s_next + ((SOURCEX >> 3) & ~1);
    vars->sform += offset;
    vars->s_next = -vars->s_next;   /* we draw from the bottom up */

#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE)
    {
        screen_blit16(vars);
        return;
    }
#endif

    /*
     * calculate the screen address
     *
     * note that the casts below allow the compiler to generate a mulu
     * instruction rather than calling _mulsi3(): this by itself speeds
     * up plain text output by about 3% ...
     */
    vars->tddad = vars->DESTX & 0x000f;
    vars->dform = v_bas_ad;
    vars->dform += (vars->DESTX&0xfff0)>>v_planes_shift;    /* add x coordinate part of addr */
    vars->dform += (UWORD)(vars->DESTY+vars->DELY-1) * (ULONG)v_lin_wr; /* add y coordinate part of addr */
    vars->d_next = -v_lin_wr;

    normal_blit(vars+1, vars->sform, vars->dform);  /* call assembler helper function */
}


/*
 * resize characters for line-A
 *
 * this is similar to act_siz(), but note that act_siz() always starts
 * with the same value in the accumulator, while here the initial value
 * is passed as an argument.
 *
 * this allows us to use the same routine for scaling height (which is
 * constant for a given string) and width (which can vary since we do a
 * kind of nano-justification).
 *
 * entry:
 *  init        initial value of accumulator
 *  size        value to scale
 *
 * used variables:
 *  DDAINC      DDA increment passed externally
 *  SCALDIR     0 if scale down, 1 if enlarge
 *
 * exit:
 *  new size
 */
static UWORD char_resize(UWORD init, UWORD size)
{
    UWORD accu, retval, i;

    if (DDAINC == 0xffff) {     /* double size */
        return (size<<1);
    }
    accu = init;
    retval = SCALDIR ? size : 0;

    for (i = 0; i < size; i++) {
        accu += DDAINC;
        if (accu < DDAINC)
            retval++;
    }

    /* if input is non-zero, make return value at least 1 */
    if (size && !retval)
        retval = 1;

    return retval;
}


void text_blt(void)
{
    LOCALVARS vars;
    WORD clipped, delx, dely, weight;
    WORD temp;
    BOOL need_preblit = FALSE;

    vars.swap_tmps = 0;

    /*
     * make local copies, just as the original code does
     */
    vars.STYLE = STYLE;
    vars.WRT_MODE = WRT_MODE;
    vars.DELX = DELX;
    vars.DESTX = DESTX;
    vars.DELY = DELY;
    vars.DESTY = DESTY;

    vars.buffa = 0;
    dely = vars.DELY;
    delx = vars.DELX;

    if (SCALE)
    {
        vars.tmp_dely = dely = char_resize(0x7fff, vars.DELY);
        vars.tmp_delx = delx = char_resize(XDDA, vars.DELX);
    }

    vars.smear = 0;
    if (vars.STYLE & F_THICKEN)
    {
        weight = WEIGHT;
        if (weight == 0)        /* cancel thicken if no weight */
            vars.STYLE &= ~F_THICKEN;
        if (!MONO)
            delx += weight;
    }

    if (vars.STYLE & F_SKEW)
    {
        delx += LOFF + ROFF;
    }

    if (vars.STYLE & F_OUTLINE)
    {
        delx += OUTLINE_THICKNESS * 2;
        dely += OUTLINE_THICKNESS * 2;
    }

    switch(CHUP) {
    case 900:
        vars.DESTY -= delx;
        FALLTHROUGH;
    case 2700:
        temp = delx;        /* swap delx/dely for 90 or 270 */
        delx = dely;
        dely = temp;
        break;
    case 1800:
        vars.DESTX -= delx;
    }

    clipped = check_clip(&vars, delx, dely);
    if (clipped < 0)
        goto upda_dst;

    vars.dest_wrd = 0;
    vars.s_next = FWIDTH;
    vars.sform = (UBYTE *)FBASE;

    if (SCALE)
    {
        scale(&vars);
    }

    /*
     * decide if we need to copy the source glyph to a temporary buffer
     * so we can manipulate it before the actual screen blit
     *
     * we copy in the following situations:
     *  (in 16-bit mode) if (skewing OR thickening OR outlining), OR
     *  if outlining, OR
     *     rotating AND (skewing OR thickening), OR
     *     skewing AND clipping-is-required,
     *      call pre_blit()
     */
#if CONF_WITH_VDI_16BIT
    if (TRUECOLOR_MODE && (vars.STYLE & (F_SKEW|F_THICKEN|F_OUTLINE)))
        need_preblit = TRUE;
    else
#endif
    if (vars.STYLE & F_OUTLINE)
        need_preblit = TRUE;
    else if (CHUP && (vars.STYLE & (F_SKEW|F_THICKEN)))
        need_preblit = TRUE;
    else if ((vars.STYLE & F_SKEW) && clipped)
        need_preblit = TRUE;

    if (need_preblit)
    {
        pre_blit(&vars);
    }

    if (CHUP)
    {
        rotate(&vars);
    }

    if (vars.STYLE & F_THICKEN)
    {
        vars.smear = WEIGHT;
        if (!MONO)
            vars.DELX += vars.smear;
    }

    if (do_clip(&vars) == 0)
    {
        screen_blit(&vars);
    }

upda_dst:
    delx = DELX;
    if (SCALE)
    {
        delx = vars.swap_tmps ? vars.tmp_dely : vars.tmp_delx;
    }

    if (STYLE & F_OUTLINE)
    {
        delx += OUTLINE_THICKNESS * 2;
    }

    if ((STYLE & F_THICKEN) && !MONO)
    {
        delx += WEIGHT;
    }

    switch(CHUP) {
    default:        /* normally 0, the default */
        DESTX += delx;      /* move right by DELX */
        break;
    case 900:
        DESTY -= delx;      /* move up by DELX */
        break;
    case 1800:
        DESTX -= delx;      /* move left by DELX */
        break;
    case 2700:
        DESTY += delx;      /* move down by DELX */
        break;
    }
}
