/*
 * vdi_main.c - the VDI screen driver dispatcher
 *
 * Copyright (C) 1999 Caldera, Inc.
 *               2002-2024 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "vdi_defs.h"
#include "lineavars.h"
#include "asm.h"
#include "string.h"

/* forward prototypes */
void screen(void);
#if CONF_WITH_VDI_FASTDRAW && !defined(__mcoldfire__)
void screen_general(void);      /* screen() is in vdi_asm.S */
#endif


WORD flip_y;                    /* True if magnitudes being returned */
extern Vwk phys_work;           /* attribute area for physical workstation */

typedef void (*VDI_OP_T)(Vwk *); /* Pointer type to VDI operation */
#define vdi_v_nop ((VDI_OP_T)just_rts) /* VDI dummy operation */

#if CONF_WITH_VDI_FASTDRAW && !defined(__mcoldfire__)
/* words, so that the rows are 8 bytes and screen() below can take them as such */
typedef WORD VDI_COUNT_T;
#else
typedef unsigned char VDI_COUNT_T;
#endif

struct vdi_jmptab {
    VDI_COUNT_T nptsout;
    VDI_COUNT_T nintout;
    VDI_OP_T op;
};

/* Two main jumptables for VDI functions */
#if CONF_WITH_VDI_FASTDRAW && !defined(__mcoldfire__)
#define JMPTB_STATIC            /* read by screen() in vdi_asm.S */
#else
#define JMPTB_STATIC static
#endif
JMPTB_STATIC struct vdi_jmptab const jmptb1[] = {
    { 6, 45, vdi_v_opnwk },            /*   1 */
    { 0,  0, vdi_v_clswk },            /*   2 */
    { 0,  0, vdi_v_clrwk },            /*   3 */
    { 0,  0, vdi_v_nop },              /*   4 - v_updwk not yet implemented */
    { 0,  0, vdi_v_escape },           /*   5 - each escape subfunction has its own call */
    { 0,  0, vdi_v_pline },            /*   6 */
    { 0,  0, vdi_v_pmarker },          /*   7 */
    { 0,  0, vdi_v_gtext },            /*   8 */
    { 0,  0, vdi_v_fillarea },         /*   9 */
    { 0,  0, vdi_v_nop },              /*  10 - v_cellarray(), not usually implemented by drivers */
    { 0,  0, vdi_v_gdp },              /*  11 */
    { 2,  0, vdi_vst_height },         /*  12 */
    { 0,  1, vdi_vst_rotation },       /*  13 */
    { 0,  0, vdi_vs_color },           /*  14 */
    { 0,  1, vdi_vsl_type },           /*  15 */
    { 1,  0, vdi_vsl_width },          /*  16 */
    { 0,  1, vdi_vsl_color },          /*  17 */
    { 0,  1, vdi_vsm_type },           /*  18 */
    { 1,  0, vdi_vsm_height },         /*  19 */
    { 0,  1, vdi_vsm_color },          /*  20 */
    { 0,  1, vdi_vst_font },           /*  21 */
    { 0,  1, vdi_vst_color },          /*  22 */
    { 0,  1, vdi_vsf_interior },       /*  23 */
    { 0,  1, vdi_vsf_style },          /*  24 */
    { 0,  1, vdi_vsf_color },          /*  25 */
    { 0,  4, vdi_vq_color },           /*  26 */
    { 0,  0, vdi_v_nop },              /*  27 - vq_cellarray, not usually implemented by drivers */
    { 0,  0, vdi_v_locator },          /*  28 */
    { 0,  0, vdi_v_nop },              /*  29 - vdi_v_valuator, not usually implemented by drivers */
    { 0,  1, vdi_v_choice },           /*  30 */
    { 0,  0, vdi_v_string },           /*  31 */
    { 0,  1, vdi_vswr_mode },          /*  32 */
    { 0,  1, vdi_vsin_mode },          /*  33 */
    { 0,  0, vdi_v_nop },              /*  34 - does not exist */
    { 1,  3, vdi_vql_attributes },     /*  35 */
    { 1,  3, vdi_vqm_attributes },     /*  36 */
    { 0,  5, vdi_vqf_attributes },     /*  37 */
    { 2,  6, vdi_vqt_attributes },     /*  38 */
    { 0,  2, vdi_vst_alignment }       /*  39 */
};

JMPTB_STATIC struct vdi_jmptab const jmptb2[] = {
    { 6, 45, vdi_v_opnvwk },           /* 100 */
    { 0,  0, vdi_v_clsvwk },           /* 101 */
    { 6, 45, vdi_vq_extnd },           /* 102 */
    { 0,  0, vdi_v_contourfill },      /* 103 */
    { 0,  1, vdi_vsf_perimeter },      /* 104 */
    { 0,  2, vdi_v_get_pixel },        /* 105 */
    { 0,  1, vdi_vst_effects },        /* 106 */
    { 2,  1, vdi_vst_point },          /* 107 */
    { 0,  2, vdi_vsl_ends },           /* 108 */
    { 0,  0, vdi_vro_cpyfm },          /* 109 */
    { 0,  0, vdi_vr_trnfm },           /* 110 */
    { 0,  0, vdi_vsc_form },           /* 111 */
    { 0,  0, vdi_vsf_udpat },          /* 112 */
    { 0,  0, vdi_vsl_udsty },          /* 113 */
    { 0,  0, vdi_vr_recfl },           /* 114 */
    { 0,  1, vdi_vqin_mode },          /* 115 */
    { 4,  0, vdi_vqt_extent },         /* 116 */
    { 3,  1, vdi_vqt_width },          /* 117 */
    { 0,  1, vdi_vex_timv },           /* 118 */ /* in vdi_misc.c */
    { 0,  1, vdi_vst_load_fonts },     /* 119 */
    { 0,  0, vdi_vst_unload_fonts },   /* 120 */
    { 0,  0, vdi_vrt_cpyfm },          /* 121 */
    { 0,  0, vdi_v_show_c },           /* 122 */
    { 0,  0, vdi_v_hide_c },           /* 123 */
    { 1,  1, vdi_vq_mouse },           /* 124 */
    { 0,  0, vdi_vex_butv },           /* 125 */ /* in vdi_mouse.c */
    { 0,  0, vdi_vex_motv },           /* 126 */ /* in vdi_mouse.c */
    { 0,  0, vdi_vex_curv },           /* 127 */ /* in vdi_mouse.c */
    { 0,  1, vdi_vq_key_s },           /* 128 */
    { 0,  0, vdi_vs_clip },            /* 129 */
    { 0, 33, vdi_vqt_name },           /* 130 */
    { 5,  2, vdi_vqt_fontinfo },       /* 131 */
#if CONF_WITH_EXTENDED_MOUSE
    { 0,  0, vdi_v_nop },              /* 132 */ /* vqt_justified (PC-GEM) */
    { 0,  0, vdi_v_nop },              /* 133 */ /* vs_grayoverride (PC-GEM/3) */
    { 0,  0, vdi_vex_wheelv }          /* 134 */ /* (Milan), also v_pat_rotate (PC-GEM/3) */
#endif
};

#define JMPTB1_ENTRIES  ARRAY_SIZE(jmptb1)
#define JMPTB2_ENTRIES  ARRAY_SIZE(jmptb2)

/* the opcodes outside the two tables above are served when this is 1 */
#define WITH_JMPTB3     (CONF_WITH_VDI_OUTLINE_FONTS || CONF_WITH_VDI_NVDI_EXT)

#if WITH_JMPTB3
/* opcodes outside 1-39 and 100-134; first matching row wins */
struct vdi_xjmptab {
    UWORD opcode;
    UWORD subop;            /* VDI_ANY_SUBOP, or the one CONTRL[5] this row takes */
    UBYTE nptsout;          /* preset into CONTRL[2]; the handler may change it */
    UBYTE nintout;          /* preset into CONTRL[4]; the handler may change it */
    VDI_OP_T op;
};
#define VDI_ANY_SUBOP 0xffffU

static const struct vdi_xjmptab jmptb3[] = {
#if CONF_WITH_VDI_OUTLINE_FONTS
    /* the NVDI and SpeedoGDOS text calls, in vdi_nvdi.c and vdi_text.c */
    { 190, VDI_ANY_SUBOP, 0,  1, vdi_vqt_char_index },
    { 229, VDI_ANY_SUBOP, 0,  3, vdi_vqt_xfntinfo },
    { 230, 100,           0, 33, vdi_vqt_name_and_id },
    { 231, VDI_ANY_SUBOP, 2,  0, vdi_vst_width },
    { 232, VDI_ANY_SUBOP, 0,  0, vdi_vqt_fontheader },
    { 234, VDI_ANY_SUBOP, 0,  4, vdi_vqt_kerns },           /* vqt_trackkern */
    { 235, VDI_ANY_SUBOP, 0,  4, vdi_vqt_kerns },           /* vqt_pairkern */
    { 236, VDI_ANY_SUBOP, 0,  1, vdi_vst_charmap },         /* also vst_map_mode */
    { 237, VDI_ANY_SUBOP, 0,  2, vdi_vst_kern },            /* also vst_track_offset */
    { 240, 4200,          4,  0, vdi_vqt_real_extent },
    { 240, VDI_ANY_SUBOP, 4,  0, vdi_vqt_extent },          /* vqt_f_extent */
    { 241, VDI_ANY_SUBOP, 0,  0, vdi_v_ftext },             /* also v_ftext_offset */
    { 243, VDI_ANY_SUBOP, 0,  1, vdi_v_getoutline },        /* also v_get_outline */
    { 244, VDI_ANY_SUBOP, 0,  0, vdi_v_nop },               /* vst_scratch */
    { 245, VDI_ANY_SUBOP, 0,  0, vdi_v_nop },               /* vst_error */
    { 246, VDI_ANY_SUBOP, 2,  1, vdi_vst_arbpt },           /* also vst_arbpt32 */
    { 247, VDI_ANY_SUBOP, 4,  0, vdi_vqt_advance },         /* also vqt_advance32 */
    { 252, VDI_ANY_SUBOP, 2,  1, vdi_vst_setsize },         /* also vst_setsize32 */
    { 253, VDI_ANY_SUBOP, 0,  1, vdi_vst_skew },
#endif
#if CONF_WITH_VDI_NVDI_EXT
    /* the NVDI colour calls, in vdi_nvdicol.c; the raster and bitmap calls are added here */
    { 138, VDI_ANY_SUBOP, 0,  0, vdi_v_setrgb },
    { 170, VDI_ANY_SUBOP, 0,  0, vdi_vr_transfer_bits },    /* in vdi_xfer.c */
    { 171, VDI_ANY_SUBOP, 4,  1, vdi_vr_clip_rects },       /* in vdi_nvdimisc.c */
    /* off-screen bitmaps: 100 and 101 with a sub-opcode are in vdi_control.c and vdi_bitmap.c */
    { 200, VDI_ANY_SUBOP, 0,  1, vdi_vs_fg_color },
    { 201, VDI_ANY_SUBOP, 0,  1, vdi_vs_bg_color },
    { 202, VDI_ANY_SUBOP, 0,  6, vdi_vq_fg_color },
    { 203, VDI_ANY_SUBOP, 0,  6, vdi_vq_bg_color },
    { 204, VDI_ANY_SUBOP, 0,  0, vdi_color_entry },         /* sets 2, 6, 6 or 4 of intout */
    { 207, VDI_ANY_SUBOP, 0,  1, vdi_vs_x_color },
    { 209, VDI_ANY_SUBOP, 0,  6, vdi_vq_x_color },
#endif
    { 0,   VDI_ANY_SUBOP, 0,  0, NULL }                     /* end of table */
};

/* the row of jmptb3 for an opcode and sub-opcode; NULL if there is none */
static const struct vdi_xjmptab *find_xjmptab(UWORD opcode, UWORD subop)
{
    const struct vdi_xjmptab *x;

    for (x = jmptb3; x->op; x++)
        if ((x->opcode == opcode) && ((x->subop == VDI_ANY_SUBOP) || (x->subop == subop)))
            return x;

    return NULL;
}
#endif /* WITH_JMPTB3 */


#if WITH_JMPTB3
/*
 * Around an OS call that can sleep: see vdi_sleep.h.  What another
 * program's VDI call would overwrite is kept in the caller's state.
 */
void vdi_sleep_begin(VdiSleepState *state)
{
    state->pb[0] = CONTRL;
    state->pb[1] = INTIN;
    state->pb[2] = PTSIN;
    state->pb[3] = INTOUT;
    state->pb[4] = PTSOUT;
    state->cur_work = CUR_WORK;     /* the pointer only: see vdi_sleep_end() */
    state->flip_y = flip_y;
    memcpy(state->ptsin, vdishare.main.local_ptsin, sizeof(state->ptsin));
    state->vwk = NULL;
#if CONF_WITH_VDI_NVDI_EXT
    vdi_bitmap_sleep_begin(state);
#endif
}

void vdi_sleep_end(VdiSleepState *state)
{
    CONTRL = state->pb[0];
    INTIN = state->pb[1];
    PTSIN = state->pb[2];
    INTOUT = state->pb[3];
    PTSOUT = state->pb[4];
    /*
     * CUR_WORK may point at a workstation closed meanwhile, or at memory
     * that was never one (a program's own Line-A array): it is not read,
     * only looked for among the open workstations.
     */
    if (state->cur_work == NULL || state->cur_work == (void *)&phys_work)
        CUR_WORK = (Vwk *)state->cur_work;
    else
    {
        WORD handle;

        for (handle = 1; handle <= NUM_VDI_HANDLES; handle++)
        {
            if (get_vwk_by_handle(handle) == (Vwk *)state->cur_work)
            {
                CUR_WORK = (Vwk *)state->cur_work;
                break;
            }
        }
    }
    flip_y = state->flip_y;
    memcpy(vdishare.main.local_ptsin, state->ptsin, sizeof(state->ptsin));
#if CONF_WITH_VDI_NVDI_EXT
    vdi_bitmap_sleep_end(state);
#endif
}
#endif /* WITH_JMPTB3 */


#if CONF_WITH_VDI_FASTDRAW && !defined(__mcoldfire__)
/*
 * The numbers screen() in vdi_asm.S uses (vdiscreen.h) are those of the
 * structures here
 */
_Static_assert(sizeof(struct vdi_jmptab) == (1 << VS_ROW_SHIFT), "vdi_jmptab row size");
_Static_assert(offsetof(struct vdi_jmptab, nptsout) == VS_ROW_NPTSOUT, "nptsout");
_Static_assert(offsetof(struct vdi_jmptab, nintout) == VS_ROW_NINTOUT, "nintout");
_Static_assert(offsetof(struct vdi_jmptab, op) == VS_ROW_OP, "op");
_Static_assert(sizeof(jmptb1[0]) == (1 << VS_ROW_SHIFT), "jmptb1 row size");
_Static_assert(ARRAY_SIZE(jmptb1) == VS_OP_TAB1_LAST - V_OPNWK_OP + 1, "jmptb1 entries");
_Static_assert(ARRAY_SIZE(jmptb2) == VS_OP_TAB2_LAST - V_OPNVWK_OP + 1, "jmptb2 entries");
_Static_assert(V_OPNWK_OP + 1 == VS_OP_CLSWK && V_CLSWK_OP == VS_OP_CLSWK, "close workstation");
_Static_assert(V_OPNVWK_OP + 1 == VS_OP_CLSVWK && V_CLSVWK_OP == VS_OP_CLSVWK, "close virtual workstation");
_Static_assert(sizeof(vwk_ptr[0]) == (1 << VS_VWKPTR_SHIFT), "vwk_ptr element size");
_Static_assert(sizeof(WORD) == 2, "CONTRL words");
_Static_assert(VS_CONTRL_OP == 0 * sizeof(WORD), "CONTRL[0]");
_Static_assert(VS_CONTRL_NPTSOUT == 2 * sizeof(WORD), "CONTRL[2]");
_Static_assert(VS_CONTRL_NINTOUT == 4 * sizeof(WORD), "CONTRL[4]");
_Static_assert(VS_CONTRL_HANDLE == 6 * sizeof(WORD), "CONTRL[6]");
_Static_assert(VS_FIS_USER == FIS_USER, "FIS_USER");
_Static_assert(offsetof(Vwk, clip) == VS_VWK_CLIP, "clip");
_Static_assert(offsetof(Vwk, cur_font) == VS_VWK_CUR_FONT, "cur_font");
_Static_assert(offsetof(Vwk, multifill) == VS_VWK_MULTIFILL, "multifill");
_Static_assert(offsetof(Vwk, fill_style) == VS_VWK_FILL_STYLE, "fill_style");
_Static_assert(offsetof(Vwk, wrt_mode) == VS_VWK_WRT_MODE, "wrt_mode");
_Static_assert(offsetof(Vwk, xmn_clip) == VS_VWK_XMN_CLIP, "xmn_clip");
_Static_assert(offsetof(Vwk, xmx_clip) == VS_VWK_XMX_CLIP, "xmx_clip");
_Static_assert(offsetof(Vwk, ymn_clip) == VS_VWK_YMN_CLIP, "ymn_clip");
_Static_assert(offsetof(Vwk, ymx_clip) == VS_VWK_YMX_CLIP, "ymx_clip");
#if CONF_WITH_VDI_OUTLINE_FONTS
_Static_assert(offsetof(Vwk, outline_head) == VS_VWK_OUTLINE_HEAD, "outline_head");
_Static_assert(offsetof(Vwk, outline_font) == VS_VWK_OUTLINE_FONT, "outline_font");
#endif
#if CONF_WITH_VDI_NVDI_EXT
_Static_assert(offsetof(Vwk, bm.addr) == VS_VWK_BM_ADDR, "bm.addr");
#endif
#endif

/*
 * screen - Screen driver entry point
 */
#if CONF_WITH_VDI_FASTDRAW && !defined(__mcoldfire__)
void screen_general(void)
#else
void screen(void)
#endif
{
    WORD opcode, handle;
    WORD *contrl = CONTRL;
    const struct vdi_jmptab *jmptab;
    Vwk *vwk = NULL;
#if WITH_JMPTB3
    const struct vdi_xjmptab *xjmptab;
    struct vdi_jmptab row;
#endif
#if CONF_WITH_VDI_NVDI_EXT
    BOOL swapped = FALSE;
#endif

    /* get workstation handle */
    handle = CONTRL[6];

    /* no ints out & no pts out */
    contrl[2] = 0;
    contrl[4] = 0;

    flip_y = 0;
    opcode = contrl[0];

    /* is it open work or vwork? */
    if ((opcode != V_OPNWK_OP) && (opcode != V_OPNVWK_OP)) {
        /* Find the vwk which matches the handle, if there */
        vwk = get_vwk_by_handle(handle);
        if (!vwk)
            return;

        /* This copying is done for assembler routines */
        if (vwk->fill_style != 4)       /* multifill just for user */
            vwk->multifill = 0;
    }

    if ((opcode >= V_OPNWK_OP) && (opcode < V_OPNWK_OP+JMPTB1_ENTRIES)) {
        jmptab = &jmptb1[opcode - V_OPNWK_OP];
    }
    else if ((opcode >= V_OPNVWK_OP) && (opcode < V_OPNVWK_OP+JMPTB2_ENTRIES)) {
        jmptab = &jmptb2[opcode - V_OPNVWK_OP];
    } else {
#if WITH_JMPTB3
        xjmptab = find_xjmptab(opcode, contrl[5]);
        if (!xjmptab)
            return;
        row.nptsout = xjmptab->nptsout;
        row.nintout = xjmptab->nintout;
        row.op = xjmptab->op;
        jmptab = &row;
#else
        return;
#endif
    }
    contrl[2] = jmptab->nptsout;
    contrl[4] = jmptab->nintout;
#if CONF_WITH_VDI_NVDI_EXT && CONF_WITH_VDI_16BIT
    /*
     * the 16-bit drawing routines read the colours of CUR_WORK: it is this
     * workstation while the call runs, not the one of the previous call
     */
    if (vwk && TRUECOLOR_MODE)
        CUR_WORK = vwk;
#endif

#if CONF_WITH_VDI_NVDI_EXT
    /* a call on the handle of an off-screen bitmap draws in it */
    if (vwk && vwk->bm.addr && !vdi_acts_on_screen(opcode))
        swapped = vdi_bitmap_swap_in(vwk);
#endif

    (*jmptab->op) (vwk);

#if CONF_WITH_VDI_NVDI_EXT
    if (swapped)
        vdi_bitmap_swap_out(opcode);
#endif

#if CONF_WITH_VDI_NVDI_EXT && CONF_WITH_VDI_16BIT
    /* line-A draws with pen 0 as the background */
    vdi_bg_text = vdi_bg_line = 0;
#endif

    /*
     * at this point, for v_opnwk() and v_opnvwk(), vwk is NULL.  we
     * must fix this before we use it to set the lineA variables below.
     * fortunately, v_opnwk() and v_opnvwk() have set CUR_WORK to a
     * valid value (see vdi_control.c).  so we use this to set vwk.
     */
    if ((opcode == V_OPNWK_OP) || (opcode == V_OPNVWK_OP))
    {
        vwk = CUR_WORK;
    }

    /*
     * set some line-A variables from the vwk info (as long as
     * the workstation is valid)
     */
    if ((opcode != V_CLSWK_OP) && (opcode != V_CLSVWK_OP)) {
        /*
         * the following assignments are not required by EmuTOS, but
         * ensure that the values in the line-A variables mirror those
         * in the current virtual workstation, just like in Atari TOS.
         */
#if CONF_WITH_VDI_OUTLINE_FONTS
        /* line-A draws bitmap fonts only: show it the system font instead */
        CUR_FONT = outline_font_selected(vwk) ? def_font : vwk->cur_font;
#else
        CUR_FONT = vwk->cur_font;
#endif
        WRT_MODE = vwk->wrt_mode;
        CLIP = vwk->clip;
        XMINCL = vwk->xmn_clip;
        YMINCL = vwk->ymn_clip;
        XMAXCL = vwk->xmx_clip;
        YMAXCL = vwk->ymx_clip;
        CUR_WORK = vwk;
    }
}
