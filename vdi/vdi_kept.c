/*
 * vdi_kept.c - memory that outlives the program making the VDI call
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "gemdos.h"
#include "cookie.h"
#include "vdi_kept.h"
#include "vdi_swap.h"

#define COOKIE_MINT     0x4d694e54L     /* 'MiNT' */
#define COOKIE_MAGX     0x4d616758L     /* 'MagX' */

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
    VdiSwapState sw;

    vdi_swap_pause(&sw);
    __asm__ volatile
    ("move.w  %2,-(%%sp)\n\t"
     "move.l  %1,-(%%sp)\n\t"
     "move.w  #0x44,-(%%sp)\n\t"
     "trap    #1\n\t"
     "lea     8(%%sp),%%sp"
    : "=r"(ret) : "r"(size), "r"(mode)
    : "d1", "d2", "a0", "a1", "a2", "cc", "memory");

    {
        LONG r = ret;

        vdi_swap_resume(&sw);
        return r;
    }
}

static void os_mfree(void *block)
{
    register LONG ret __asm__("d0");
    VdiSwapState sw;

    vdi_swap_pause(&sw);
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

