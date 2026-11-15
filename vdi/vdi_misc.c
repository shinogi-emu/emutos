/*
 * vdi_misc.c - everything, what does not fit in elsewhere
 *
 * Copyright 1982 by Digital Research Inc.  All rights reserved.
 * Copyright 1999 by Caldera, Inc. and Authors:
 * Copyright 2002-2021 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "asm.h"
#include "intmath.h"
#include "biosbind.h"
#include "tosvars.h"
#include "vdi_defs.h"
#include "lineavars.h"
#include "biosext.h"
#include "gemdos.h"
#include "cookie.h"

static BOOL in_proc;                   /* flag, if we are still running */



/*
 * arb_corner - copy and sort (arbitrate) the corners
 *
 * raster (ll, ur) format is desired.
 */
void arb_corner(Rect * rect)
{
    /* Fix the x coordinate values, if necessary. */
    if (rect->x1 > rect->x2) {
        WORD temp = rect->x1;
        rect->x1 = rect->x2;
        rect->x2 = temp;
    }

    /* Fix the y coordinate values, if necessary. */
    if (rect->y1 > rect->y2) {
        WORD temp = rect->y1;
        rect->y1 = rect->y2;
        rect->y2 = temp;
    }
}



/*
 * arb_line - copy and sort (arbitrate) the lines coordinates
 *
 * traditional (ll, ur) format is desired.
 */
void arb_line(Line * line)
{
    /* Fix the x coordinate values, if necessary. */
    if (line->x1 > line->x2) {
        WORD temp = line->x1;
        line->x1 = line->x2;
        line->x2 = temp;
    }

    /* Fix the y coordinate values, if necessary. */
    if (line->y1 < line->y2) {
        WORD temp = line->y1;
        line->y1 = line->y2;
        line->y2 = temp;
    }
}



/*
 * tick_int -  VDI Timer interrupt routine
 *
 * The etv_timer does point to this routine
 */
static void tick_int(int u)
{
    if (!in_proc) {
        in_proc = 1;                    /* set flag, that we are running */
                                        /* MAD: evtl. registers to stack */
        tim_addr(u);                    /* call the timer vector */
                                        /* and back from stack */
    }
    in_proc = 0;                        /* allow yet another trip through */
                                        /* MAD: evtl. registers to stack */
    tim_chain(u);                       /* call the old timer vector too */
                                        /* and back from stack */
}



/*
 * vdi_vex_timv - exchange timer interrupt vector
 *
 * entry:          new vector in CONTRL[7-8]
 * exit:           old vector in CONTRL[9-10]
 */
void vdi_vex_timv(Vwk * vwk)
{
    WORD old_sr;

    old_sr = set_sr(0x2700);

    ULONG_AT(&CONTRL[9]) = (ULONG) tim_addr;
    tim_addr = (ETV_TIMER_T) ULONG_AT(&CONTRL[7]);

    set_sr(old_sr);

    INTOUT[0] = (WORD)Tickcal();        /* ms between timer C calls */
}



/*
 * timer_init - initialize the timer
 *
 * initially set timer vector to dummy, save old vector
 */
void timer_init(void)
{
    WORD old_sr;

    in_proc = 0;                        /* no vblanks in process */

    /* Now initialize the lower level things */
    tim_addr = (ETV_TIMER_T)just_rts;   /* tick points to rts */

    old_sr = set_sr(0x2700);            /* disable interrupts */
    tim_chain = (ETV_TIMER_T)           /* save old vector */
        Setexc(0x100, (long)tick_int);  /* set etv_timer to tick_int */
    set_sr(old_sr);                     /* enable interrupts */

}



/*
 * timer_exit - de-initialize the time
 *
 * reactivate the old saved vector
 */
void timer_exit(void)
{
    WORD old_sr;

    old_sr = set_sr(0x2700);            /* disable interrupts */
    Setexc(0x100, (long)tim_chain);     /* set etv_timer to tick_int */
    set_sr(old_sr);                     /* enable interrupts */
}



/*
 * get_start_addr - return memory address for column x, row y
 *
 * NOTE: the input x value may be negative (for example, this happens
 * when handling a slanting wideline starting at pixel 0 of a row).  This
 * value must be right-shifted to obtain an offset in bytes.
 * According to the C standard, the result of right-shifting a negative
 * value is implementation-defined.  GCC has the correct behaviour from
 * our point of view: high-order bits are 1-filled, so the number remains
 * negative.
 */
UWORD * get_start_addr(const WORD x, const WORD y)
{
    UBYTE * addr;
    WORD x2 = x & 0xfff0;   /* ensure that value to be shifted remains signed! */

    /* init address counter */
    addr = v_bas_ad;                    /* start of screen */
    addr += x2 >> v_planes_shift;       /* add x coordinate part of addr */
    addr += muls(y, v_lin_wr);          /* add y coordinate part of addr */

    return (UWORD*)addr;
}


#if CONF_WITH_VDI_OUTLINE_FONTS || CONF_WITH_VDI_NVDI_EXT

/*
 * Mxalloc() mode used under MiNT and MagiC: prefer TT-RAM, keep the block
 * when its owner terminates (0x4000) and make it accessible from supervisor
 * mode only (0x30), as fVDI does for its own memory.
 */
#define MX_KEEP_MODE    (MX_PREFTTRAM | 0x4030)

/* the same, but readable by every program (0x40), as fVDI's cookie data */
#define MX_READABLE_MODE (MX_PREFTTRAM | 0x4040)

/*
 * GEMDOS with a trap of its own: trap1() keeps its caller's return address
 * and two registers in static variables, and under a multitasking GEMDOS a
 * call can sleep while another program's VDI call goes through trap1().
 * These keep nothing outside the stack.  GEMDOS may change d0-d2 and a0-a2.
 */
static LONG os_mxalloc(LONG size, WORD mode)
{
    register LONG ret __asm__("d0");

    __asm__ volatile
    ("move.w  %2,-(%%sp)\n\t"
     "move.l  %1,-(%%sp)\n\t"
     "move.w  #0x44,-(%%sp)\n\t"
     "trap    #1\n\t"
     "lea     8(%%sp),%%sp"
    : "=r"(ret) : "r"(size), "r"(mode)
    : "d1", "d2", "a0", "a1", "a2", "cc", "memory");

    return ret;
}

static void os_mfree(void *block)
{
    register LONG ret __asm__("d0");

    __asm__ volatile
    ("move.l  %1,-(%%sp)\n\t"
     "move.w  #0x49,-(%%sp)\n\t"
     "trap    #1\n\t"
     "addq.l  #6,%%sp"
    : "=r"(ret) : "r"(block)
    : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

/*
 * TRUE under MiNT or MagiC: their GEMDOS has its own ideas about who owns
 * a block
 */
BOOL vdi_other_gemdos(void)
{
    ULONG value;

    return cookie_get(COOKIE_MINT, &value) || cookie_get(COOKIE_MAGX, &value);
}


/*
 * allocate a block that lives until it is freed, whoever frees it
 *
 * The memory is shared by all programs: the caches the VDI keeps (sized
 * fonts, glyphs, work buffers) must not go when the program that happened
 * to make the VDI call terminates.
 *
 * Under MiNT or MagiC the mode of Mxalloc() does it.  EmuTOS's own GEMDOS
 * records the calling process as owner of every block and frees them when
 * it terminates, so the block is given to the initial process: that is
 * the one running before the first program starts, and it never terminates.
 * Mfree() does not check the owner, so any process can free the block.
 */
void *vdi_alloc_kept(LONG size)
{
    void *block;

    if (vdi_other_gemdos())
    {
        block = (void *)os_mxalloc(size, MX_KEEP_MODE);
        if ((LONG)block <= 0)
            return NULL;
        return block;
    }

    block = (void *)os_mxalloc(size, MX_PREFTTRAM);
    if ((LONG)block <= 0)
        return NULL;
    bdos_keep_block(block);

    return block;
}


void vdi_free_kept(void *block)
{
    os_mfree(block);
}


/*
 * a block that lives until it is freed and that every program can read,
 * for data programs are given a pointer to (the NVDI cookie): under MiNT
 * with memory protection, the VDI's blocks are for supervisor mode only
 */
void *vdi_alloc_readable(LONG size)
{
    void *block;

    if (!vdi_other_gemdos())
        return vdi_alloc_kept(size);    /* EmuTOS's own GEMDOS protects nothing */

    block = (void *)os_mxalloc(size, MX_READABLE_MODE);
    if ((LONG)block <= 0)
        return NULL;

    return block;
}

#endif /* CONF_WITH_VDI_OUTLINE_FONTS || CONF_WITH_VDI_NVDI_EXT */
