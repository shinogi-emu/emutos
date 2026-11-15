/*
 * vdi_sleep.h - OS calls that can sleep, made from inside a VDI call
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_SLEEP_H
#define VDI_SLEEP_H

#include "portab.h"

/*
 * Contract for the OS glue of the VDI (vdi_ftos.c, vdi_kept.c, or another
 * host's replacements).
 *
 * The VDI keeps the state of the call in progress in variables of its own:
 * the parameter block pointers (CONTRL, INTIN, PTSIN, INTOUT, PTSOUT), the
 * current workstation (CUR_WORK), the answer of the call (flip_y), the
 * first points of its copy of PTSIN (vdishare), and, while it draws in an
 * off-screen bitmap, the screen variables (they describe the bitmap).
 * Supervisor code is not preempted by a multitasking OS except where it
 * sleeps; but an OS call that sleeps lets another program's VDI call run,
 * which overwrites all of that.  So every OS call that can sleep (GEMDOS
 * file, folder and memory calls, anything that waits) made from the VDI
 * must be bracketed, on every path out of it:
 *
 *     VdiSleepState sw;
 *
 *     vdi_sleep_begin(&sw);
 *     ... the OS call ...
 *     vdi_sleep_end(&sw);
 *
 * Begin keeps that state in sw and puts the screen variables back (when a
 * bitmap is in use); end puts it all back as the call left it, and the
 * bitmap's screen variables again over whatever they are then.  The state
 * is the caller's own, on its stack: the program that sleeps is not the
 * only one that may swap, and two programs can each sleep inside a call.
 * Do not make one OS call from the middle of another bracket; make the
 * pair part of the one function that makes the OS call, so that no path
 * can miss the end.  The pair works outside a VDI call too (the NVDI font
 * vectors, vq_vgdos), and the OS call itself must keep nothing in statics
 * across its sleep (hence the traps of vdi_ftos.c and vdi_kept.c, not
 * trap1()).  A static of the VDI that is set before an OS call and read
 * after it is not saved: no code may do that (set it after the call).
 * Begin does not read through CUR_WORK: it may point at a workstation
 * that is closed, or at a Line-A program's own array.  End puts it back
 * only if it is NULL, the physical workstation or still one of the open
 * workstations (compared as a pointer); otherwise it leaves CUR_WORK as
 * it finds it.
 *
 * Known limits: the waits of request-mode input (Bconin() in v_choice,
 * v_string, vrq_locator) and of the escapes' Bconout() are not bracketed;
 * they are in calls that act on the screen and the text console, which the
 * bitmaps do not touch, and they block the VDI as they always did.
 *
 * Not saved: the rest of vdishare (the PTSIN points past the fourth, the
 * fill buffer, the text scratch buffer), which no call holds across an OS
 * call.  vdi_v_ftext() keeps its offsets in a buffer of its own for that
 * reason.
 *
 * With both outline fonts and the NVDI extensions off the VDI makes no OS
 * calls that sleep and both are empty.
 */
#if CONF_WITH_VDI_OUTLINE_FONTS || CONF_WITH_VDI_NVDI_EXT
typedef struct {
    WORD *pb[5];                /* CONTRL, INTIN, PTSIN, INTOUT, PTSOUT */
    void *cur_work;             /* compared, never read through */
    WORD flip_y;
    WORD ptsin[8];              /* the first four points of the copy of PTSIN */
    void *vwk;                  /* the bitmap workstation in use, or NULL */
    WORD handle;
} VdiSleepState;

void vdi_sleep_begin(VdiSleepState *state);
void vdi_sleep_end(VdiSleepState *state);
#else
typedef struct {
    WORD unused;
} VdiSleepState;

#define vdi_sleep_begin(state)  ((void)(state))
#define vdi_sleep_end(state)    ((void)(state))
#endif

#endif /* VDI_SLEEP_H */
