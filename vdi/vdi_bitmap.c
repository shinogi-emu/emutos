/*
 * vdi_bitmap.c - off-screen bitmaps: v_opnbm, v_open_bm, v_resize_bm and
 * v_clsbm
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * A bitmap workstation is an ordinary virtual workstation whose Vwk has
 * a bitmap description (Vwk.bm, with a non-NULL address).  Bitmaps are in
 * the format of the screen.
 *
 * To draw in one, screen() makes the screen variables describe the bitmap
 * for the length of the call (vdi_bitmap_swap_in), then puts the screen's
 * back (vdi_bitmap_swap_out).  Nothing that runs at interrupt time may
 * read them in between: the mouse cursor is not drawn (mouse_flag is
 * raised), and the other readers look at vdi_screen_swapped.
 *
 * The calls (opcode 100 with sub-opcode 1, 2 or 3 in vdi_control.c) are
 * the EdDI v_opnbm and the NVDI v_open_bm and v_resize_bm, as fVDI does
 * them; v_clsbm is v_clsvwk, which frees the memory of a bitmap.
 */

#include "emutos.h"
#include "lineavars.h"
#include "vdi_defs.h"
#include "has.h"
#include "bdosbind.h"
#include "bdosdefs.h"
#include "string.h"
#include "tosvars.h"
#include "gsxdefs.h"
#include "vdi_sleep.h"
#include "vdi_kept.h"

#if CONF_WITH_VDI_NVDI_EXT

extern Vwk phys_work;           /* attribute area for physical workstation */


UBYTE vdi_screen_swapped;       /* see lineavars.h */
WORD vdi_bm_xres, vdi_bm_yres;  /* the size of the bitmap in use, less one */

#define BM_MAX_WIDTH    32752L  /* pixels (a multiple of 16) */
#define BM_MAX_HEIGHT   32767L
#define BM_MAX_LINE     32766L  /* bytes in a line: v_lin_wr is multiplied as a WORD */
#define BM_SPARE        16L     /* bytes of the block before and after the pixels */
#define BM_ALIGN        16L

/* Mxalloc() protection bits: the block can be read by every program */
#define MX_READABLE     (4<<4)


/*
 * The memory of bitmaps: the one place that asks the OS for it.
 *
 * It is a kept block, like the Vwk: it may be freed from another
 * program's call after its owner has ended, and leaks if the owner ends
 * with the bitmap open.  The program reads the pixels directly, so under MiNT the
 * block is readable by every program.  With a blitter the block must be
 * in ST-RAM, for the blitter to reach it.
 */
static void *bm_alloc(LONG size)
{
    return vdi_alloc_kept_mode(size, (HAS_BLITTER ? MX_STRAM : MX_PREFTTRAM) | MX_READABLE);
}

static BOOL bm_free(void *block)
{
    return vdi_os_mfree(block) == 0;
}


/* a LONG of two words of CONTRL or INTIN, whatever their alignment */
static ULONG long_at(const WORD *w)
{
    return ((ULONG)(UWORD)w[0] << 16) | (UWORD)w[1];
}


/* bytes in a line of a bitmap width pixels wide (a multiple of 16); 0 if too many */
static LONG line_bytes(LONG width)
{
    LONG bytes = (width / 16) * 2 * (LONG)v_planes;

    return (bytes <= BM_MAX_LINE) ? bytes : 0;
}


/*
 * describe a bitmap of width x height pixels in bm, in the format of the
 * screen.  'addr' is the caller's memory with 'wbytes' bytes to a line (0:
 * the least that holds the width), or NULL: the memory is allocated here,
 * white, and a block with 16 spare bytes each side, 16-byte aligned.
 * Returns FALSE, with nothing allocated, if it cannot be done.
 */
static BOOL bm_build(VwkBitmap *bm, LONG width, LONG height, UBYTE *addr, LONG wbytes)
{
    LONG least;
    ULONG size;
    UBYTE *block;

    if (width < 1 || width > BM_MAX_WIDTH || height < 1 || height > BM_MAX_HEIGHT)
        return FALSE;
    width = (width + 15) & ~15L;
    least = line_bytes(width);
    if (least == 0)
        return FALSE;

    if (addr)
    {
        if (wbytes == 0)
            wbytes = least;
        if ((wbytes < least) || (wbytes > BM_MAX_LINE) || (wbytes & 1) || ((ULONG)addr & 1))
            return FALSE;
        bm->mem = NULL;
        bm->addr = addr;
    }
    else
    {
        wbytes = least;
        size = (ULONG)wbytes * (ULONG)height;       /* at most about 2^30 */
        block = (UBYTE *)bm_alloc((LONG)(size + 2 * BM_SPARE + BM_ALIGN - 1));
        if (block == NULL)
            return FALSE;
        bm->mem = block;
        bm->addr = (UBYTE *)(((ULONG)block + BM_SPARE + BM_ALIGN - 1) & ~(ULONG)(BM_ALIGN - 1));
        memset(bm->addr, (v_planes > 8) ? 0xff : 0x00, size);
    }
    bm->width = (WORD)width;
    bm->height = (WORD)height;
    bm->wbytes = (UWORD)wbytes;

    return TRUE;
}


/* the size of a pixel of the workstation, in microns */
static void pixel_size_of(const Vwk *vwk, WORD *w, WORD *h)
{
    if (vwk && vwk->bm.addr)
    {
        *w = vwk->bm.pixw;
        *h = vwk->bm.pixh;
    }
    else
    {
        *w = DEV_TAB[3];
        *h = DEV_TAB[4];
    }
}


/*
 * open a virtual workstation as v_opnvwk does.  intin is the 11 words to
 * open it with (NULL: those of the call), and where the 45 + 12 words it
 * answers go (the call's own if intin is NULL).  Returns it, or NULL.
 */
static Vwk *open_vwk(const WORD *intin, WORD *intout, WORD *ptsout)
{
    WORD *old_in = INTIN, *old_out = INTOUT, *old_pts = PTSOUT;
    WORD old_n = CONTRL[3], old_sub = CONTRL[5];
    Vwk *vwk;

    if (intin)
    {
        INTIN = (WORD *)intin;
        INTOUT = intout;
        PTSOUT = ptsout;
    }
    CONTRL[5] = 0;
    vdi_v_opnvwk(NULL);
    vwk = get_vwk_by_handle(CONTRL[6]);     /* CONTRL[6] is 0 if it was refused */
    INTIN = old_in;
    INTOUT = old_out;
    PTSOUT = old_pts;
    CONTRL[3] = old_n;
    CONTRL[5] = old_sub;

    return vwk;
}


/* make vwk the workstation of bitmap bm; nothing of it may be drawn outside */
static void attach(Vwk *vwk, const VwkBitmap *bm)
{
    vwk->bm = *bm;
    vwk->xmn_clip = 0;
    vwk->ymn_clip = 0;
    vwk->xmx_clip = bm->width - 1;
    vwk->ymx_clip = bm->height - 1;
    vwk->clip = FALSE;
}


/* the call was refused: no handle */
static void refuse(void)
{
    CONTRL[6] = 0;
    CONTRL[2] = 0;
    CONTRL[4] = 0;
}


/*
 * v_opnbm: 100, sub-opcode 1.  intin[0..10] are the work_in of
 * v_opnvwk, intin[11..12] the size less one (0: the screen's), [13..14]
 * the size of a pixel in microns (0: the screen's), [15..16] the number of
 * colours, [17] the planes, [18] the pixel format and [19] the bit order
 * of EdDI 1.1.  CONTRL[7..8] points to an MFDB, which gets the memory
 * when it has none, and is filled in.  Answers as v_opnvwk.
 */
static void open_eddi(void)
{
    const WORD *in = INTIN;
    LONG planes = v_planes;
    LONG colors, width, height;
    WORD pw, ph;
    FDB *mfdb;
    Vwk *base, *vwk;
    VwkBitmap bm;

    mfdb = (FDB *)long_at(&CONTRL[7]);
    base = get_vwk_by_handle(CONTRL[6]);
    if (mfdb == NULL || base == NULL)
        { refuse(); return; }

    colors = (LONG)long_at(&in[15]);
    if (colors != 0 && colors != 2 && colors != (1L << planes))
        { refuse(); return; }
    if ((mfdb->fd_nplanes != 0 && mfdb->fd_nplanes != planes) ||
        (in[17] != 0 && in[17] != planes))
        { refuse(); return; }
    if ((in[18] != 0 && in[18] != 1) || (in[19] != 0 && in[19] != 1))
        { refuse(); return; }
    if (in[11] < 0 || in[12] < 0)
        { refuse(); return; }

    width = in[11] ? (LONG)in[11] + 1 : (LONG)DEV_TAB[0] + 1;
    height = in[12] ? (LONG)in[12] + 1 : (LONG)DEV_TAB[1] + 1;
    if (!bm_build(&bm, width, height, (UBYTE *)mfdb->fd_addr, 0))
        { refuse(); return; }

    pixel_size_of(base, &pw, &ph);
    bm.pixw = (in[13] > 0) ? in[13] : pw;
    bm.pixh = (in[14] > 0) ? in[14] : ph;

    vwk = open_vwk(NULL, NULL, NULL);
    if (vwk == NULL)
    {
        if (bm.mem)
            bm_free(bm.mem);
        { refuse(); return; }
    }
    attach(vwk, &bm);

    mfdb->fd_addr = bm.addr;
    mfdb->fd_w = bm.width;
    mfdb->fd_h = bm.height;
    mfdb->fd_wdwidth = bm.wbytes / (2 * v_planes);
    mfdb->fd_stand = 0;
    mfdb->fd_nplanes = v_planes;
    mfdb->fd_r1 = mfdb->fd_r2 = mfdb->fd_r3 = 0;

    /* the work_out answers of the workstation the bitmap is, not the screen */
    INTOUT[0] = bm.width - 1;
    INTOUT[1] = bm.height - 1;
    INTOUT[3] = bm.pixw;
    INTOUT[4] = bm.pixh;
    CONTRL[2] = 6;
    CONTRL[4] = 45;
}


/* the size of a pixel for v_open_bm: 0 as a result means the base's */
static WORD micron_size(WORD value, LONG unit)
{
    LONG v = value;
    LONG div;

    if (unit == -1)                 /* dots per inch */
        return (WORD)((25400L + v / 2) / v);
    if (unit == 0)
        return value;
    div = (unit == 1) ? 10 : (unit == 2) ? 100 : 1000;
    return (WORD)((v + div / 2) / div);
}


/*
 * v_open_bm: 100, sub-opcode 3.  CONTRL[7..8] points to a GCBITMAP (NULL:
 * the size of the base workstation), intin[0] is the colour flags (bit 0
 * set: the bitmap shares the colour table of the base workstation, clear:
 * it has one of its own).  EmuTOS has no shared tables: either way the
 * bitmap starts with a copy of the colours of the base workstation as
 * they are now, as fVDI does.  intin[1] the unit of the pixel size in
 * intin[2..3]: -1 dots per inch, 0 to 3 microns, tenths, hundredths and
 * thousandths of a micron.  Only the handle is returned.
 */
static void open_bm(void)
{
    static const WORD defaults[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    WORD intout[45], ptsout[12];
    const WORD *in = INTIN;
    ULONG format = vdi_px_format();
    GCBITMAP *gc;
    Vwk *base, *vwk;
    LONG width = (LONG)DEV_TAB[0] + 1, height = (LONG)DEV_TAB[1] + 1;
    LONG wbytes = 0;
    UBYTE *addr = NULL;
    WORD pw, ph, base_handle;
    VwkBitmap bm;

    if (CONTRL[3] < 4 || (in[0] & ~1))
        { refuse(); return; }
    base_handle = CONTRL[6];
    base = get_vwk_by_handle(base_handle);
    if (base == NULL)
        { refuse(); return; }
    if (base->bm.addr)
    {
        /* no size given: that of the base bitmap */
        width = base->bm.width;
        height = base->bm.height;
    }
    gc = (GCBITMAP *)long_at(&CONTRL[7]);

    if (gc)
    {
        if ((gc->px_format != 0 && gc->px_format != format) ||
            (gc->bits != 0 && gc->bits != (LONG)v_planes))
            { refuse(); return; }
        if (gc->addr || gc->xmin || gc->ymin || gc->xmax || gc->ymax)
        {
            /* far outside any bitmap, and the differences cannot overflow */
            if (gc->xmin < -0x10000L || gc->xmin > 0x10000L || gc->xmax < -0x10000L ||
                gc->xmax > 0x10000L || gc->ymin < -0x10000L || gc->ymin > 0x10000L ||
                gc->ymax < -0x10000L || gc->ymax > 0x10000L)
                { refuse(); return; }
            width = gc->xmax - gc->xmin;
            height = gc->ymax - gc->ymin;
        }
        if (gc->addr)
        {
            addr = gc->addr;
            wbytes = gc->width;
            if (wbytes <= 0)
                { refuse(); return; }
        }
    }

    pixel_size_of(base, &pw, &ph);
    if (in[2] > 0 && in[3] > 0)
    {
        if (in[1] < -1 || in[1] > 3)
            { refuse(); return; }
        pw = micron_size(in[2], in[1]);
        ph = micron_size(in[3], in[1]);
        if (pw <= 0 || ph <= 0)
            pixel_size_of(base, &pw, &ph);
    }

    if (!bm_build(&bm, width, height, addr, wbytes))
        { refuse(); return; }
    bm.pixw = pw;
    bm.pixh = ph;

    vwk = open_vwk(defaults, intout, ptsout);
    if (vwk == NULL)
    {
        if (bm.mem)
            bm_free(bm.mem);
        { refuse(); return; }
    }
    attach(vwk, &bm);

#if CONF_WITH_VDI_16BIT
    /* the colours of the base workstation as they are now (it may be gone if the open slept) */
    base = get_vwk_by_handle(base_handle);
    if (vwk->ext && base && base->ext)
        memcpy(vwk->ext, base->ext, sizeof(VwkExt));
#endif

    if (gc && gc->addr == NULL)
    {
        /* the memory is the VDI's: describe the bitmap */
        gc->magic = GCBITMAP_MAGIC;
        gc->length = sizeof(GCBITMAP);
        gc->format = 0;
        gc->reserved = 0;
        gc->addr = bm.addr;
        gc->width = bm.wbytes;
        gc->bits = v_planes;
        gc->px_format = format;
        gc->xmin = 0;
        gc->ymin = 0;
        gc->xmax = bm.width;
        gc->ymax = bm.height;
        gc->ctab = NULL;
        gc->itab = NULL;
        gc->reserved0 = 0;
        gc->reserved1 = 0;
    }

    CONTRL[2] = 0;
    CONTRL[4] = 0;
}


/*
 * v_resize_bm: 100, sub-opcode 2, CONTRL[6] the handle of the bitmap.
 * intin[0..1] the new size, intin[2..3] the bytes to a line and intin[4..5]
 * the address of memory of the caller's (0: memory of the VDI's).  intout[0]
 * is 1 when it was done, and 0 when not, with the bitmap as it was.
 */
static void resize_bm(void)
{
    const WORD *in = INTIN;
    Vwk *vwk = get_vwk_by_handle(CONTRL[6]);
    VwkBitmap nb;
    UBYTE *addr = NULL;
    LONG wbytes = 0;
    LONG rows, bytes, r;

    CONTRL[2] = 0;
    CONTRL[4] = 1;
    INTOUT[0] = 0;
    if (vwk == NULL || vwk->bm.addr == NULL || CONTRL[3] < 4)
        return;

    if (CONTRL[3] >= 6)
    {
        wbytes = (LONG)long_at(&in[2]);
        addr = (UBYTE *)long_at(&in[4]);
        if (addr && wbytes <= 0)
            return;
    }
    if (in[0] <= 0 || in[1] <= 0)
        return;
    if (!bm_build(&nb, in[0], in[1], addr, wbytes))
        return;

    nb.pixw = vwk->bm.pixw;
    nb.pixh = vwk->bm.pixh;
    if (nb.mem)
    {
        /* the old pixels where the two overlap */
        rows = (nb.height < vwk->bm.height) ? nb.height : vwk->bm.height;
        bytes = (nb.wbytes < vwk->bm.wbytes) ? nb.wbytes : vwk->bm.wbytes;
        for (r = 0; r < rows; r++)
            memcpy(nb.addr + r * nb.wbytes, vwk->bm.addr + r * vwk->bm.wbytes, bytes);
    }
    if (vwk->bm.mem)
        bm_free(vwk->bm.mem);
    vwk->bm = nb;

    /* the clipping rectangle stays inside the new size */
    if (vwk->clip)
    {
        if (vwk->xmx_clip > nb.width - 1)
            vwk->xmx_clip = nb.width - 1;
        if (vwk->ymx_clip > nb.height - 1)
            vwk->ymx_clip = nb.height - 1;
        if (vwk->xmn_clip > vwk->xmx_clip)
            vwk->xmn_clip = vwk->xmx_clip;
        if (vwk->ymn_clip > vwk->ymx_clip)
            vwk->ymn_clip = vwk->ymx_clip;
    }
    else
    {
        vwk->xmx_clip = nb.width - 1;
        vwk->ymx_clip = nb.height - 1;
    }
    INTOUT[0] = 1;
}


/*
 * 100 with sub-opcode 1 (CONTRL[3] >= 20), 2 or 3: the bitmap calls (any
 * other sub-opcode opens a workstation, as v_opnvwk).  CUR_WORK is left
 * pointing at a valid workstation, as screen() reads it afterwards.
 */
void vdi_bitmap_open(void)
{
    WORD sub = CONTRL[5];

    CUR_WORK = &phys_work;
    CONTRL[2] = 0;
    CONTRL[4] = 0;
    if (sub == 1)
        open_eddi();
    else if (sub == 2)
        resize_bm();
    else
        open_bm();
}


/* release_vwk() in vdi_main.c calls this before it frees a closed workstation,
   perhaps from another program's call: the pixels are kept blocks */
void vdi_bitmap_release(Vwk *vwk)
{
    if (vwk->bm.mem)
        bm_free(vwk->bm.mem);
    vwk->bm.mem = NULL;
    vwk->bm.addr = NULL;
}


/*
 * the opcodes that act on the screen even with the handle of a bitmap:
 * the input and mouse calls, which reach for the hardware, the opening and
 * closing of workstations, and the escapes (the text screen)
 */
BOOL vdi_acts_on_screen(WORD opcode)
{
    return (opcode == 2) || (opcode == 5) ||
           (opcode >= 28 && opcode <= 31) || (opcode == 33) ||
           (opcode == 100) || (opcode == 101) ||
           (opcode == 111) || (opcode == 115) || (opcode == 118) ||
           (opcode >= 122 && opcode <= 128) || (opcode == 134);
}


#define VS_CLIP_OP      129

/* the swap now in force: what it replaced, and the workstation drawn in */
static struct {
    Vwk *vwk;
    UBYTE *bas_ad;
    UWORD lin_wr;
    UWORD bytes_lin;
    UWORD rez_hz;
    UWORD rez_vt;
    WORD pixw;
    WORD pixh;
    UBYTE *screenpt;
    WORD clip;                  /* the caller's clipping flag */
} saved;

WORD vdi_bm_caller_clip;        /* the same, for vq_extnd */

#define BARRIER() __asm__ volatile ("" : : : "memory")


/*
 * Put the screen variables to the bitmap of vwk.
 *
 * The mouse cursor is not drawn meanwhile (mouse_flag), and the blink and
 * the GPU presentation stand by (vdi_screen_swapped).  The VBL must not
 * copy screenpt to v_bas_ad: it is zero while swapped.  DEV_TAB[0] and [1]
 * stay the screen's (the mouse clips to them): xres and yres give the
 * bitmap's.  A call that draws with clipping off would write past the
 * bitmap: it clips to the bitmap instead (the rectangle of a workstation
 * with clipping off is the whole bitmap).
 *
 * Known limit: interrupt code that a program installed (a cursor routine
 * of vex_curv called by the mouse interrupt, vblqueue entries, dump_vec,
 * vex_timv) and that reads the Line-A screen variables sees the bitmap's
 * values during the call.
 */
static void apply(Vwk *vwk)
{
    const VwkBitmap *bm = &vwk->bm;

    mouse_flag++;
    vdi_bm_xres = bm->width - 1;
    vdi_bm_yres = bm->height - 1;
    vdi_screen_swapped = 1;
    BARRIER();

    saved.vwk = vwk;
    saved.bas_ad = v_bas_ad;
    saved.lin_wr = v_lin_wr;
    saved.bytes_lin = BYTES_LIN;
    saved.rez_hz = V_REZ_HZ;
    saved.rez_vt = V_REZ_VT;
    saved.pixw = DEV_TAB[3];
    saved.pixh = DEV_TAB[4];
    saved.screenpt = screenpt;
    saved.clip = vwk->clip;
    vdi_bm_caller_clip = vwk->clip;

    screenpt = NULL;
    v_bas_ad = bm->addr;
    v_lin_wr = bm->wbytes;
    BYTES_LIN = bm->wbytes;
    V_REZ_HZ = bm->width;
    V_REZ_VT = bm->height;
    DEV_TAB[3] = bm->pixw;
    DEV_TAB[4] = bm->pixh;
    vwk->clip = TRUE;
}


/* the screen's again; the caller's clipping flag, unless the call set it */
static void undo(BOOL keep_clip)
{
    if (!keep_clip)
        saved.vwk->clip = saved.clip;
    v_bas_ad = saved.bas_ad;
    v_lin_wr = saved.lin_wr;
    BYTES_LIN = saved.bytes_lin;
    V_REZ_HZ = saved.rez_hz;
    V_REZ_VT = saved.rez_vt;
    DEV_TAB[3] = saved.pixw;
    DEV_TAB[4] = saved.pixh;
    if (screenpt == NULL)       /* unless something set a new one meanwhile */
        screenpt = saved.screenpt;

    BARRIER();
    vdi_screen_swapped = 0;
    mouse_flag--;
    saved.vwk = NULL;
}


BOOL vdi_bitmap_swap_in(Vwk *vwk)
{
    if (vwk->bm.addr == NULL)
        return FALSE;
    apply(vwk);
    return TRUE;
}


void vdi_bitmap_swap_out(WORD opcode)
{
    if (saved.vwk == NULL)      /* nothing applied: the swap was not resumed */
        return;
    undo(opcode == VS_CLIP_OP);
}


/*
 * Before an OS call that can sleep (vdi_sleep_begin() in vdi_main.c): the
 * screen's variables back, the state of the swap kept in the caller's.
 * Another program's VDI call may run meanwhile, and may swap itself.
 */
void vdi_bitmap_sleep_begin(VdiSleepState *state)
{
    if (!vdi_screen_swapped)
    {
        state->vwk = NULL;
        return;
    }
    state->vwk = saved.vwk;
    state->handle = saved.vwk->handle;
    undo(FALSE);
}


/*
 * After it: the swap again, over whatever the screen variables are now
 * (the swap of a program that was asleep too was undone before it slept).
 * The workstation may have been closed meanwhile: then nothing is applied,
 * vdi_screen_swapped and mouse_flag stay as they are, and swap_out() at
 * the end of the call does nothing.
 */
void vdi_bitmap_sleep_end(VdiSleepState *state)
{
    if (state->vwk && get_vwk_by_handle(state->handle) == (Vwk *)state->vwk &&
        ((Vwk *)state->vwk)->bm.addr)
        apply((Vwk *)state->vwk);
}

#endif /* CONF_WITH_VDI_NVDI_EXT */
