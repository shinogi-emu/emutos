/*
 * vdi_swap.h - sleeping OS calls while the VDI draws in an off-screen bitmap
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_SWAP_H
#define VDI_SWAP_H

#include "portab.h"

/*
 * Contract for the OS glue of the VDI (vdi_ftos.c, vdi_kept.c, or another
 * host's replacements).
 *
 * While a VDI call draws in an off-screen bitmap, the variables that
 * describe the screen describe the bitmap (vdi_bitmap.c).  That is only
 * safe because nothing else runs meanwhile: supervisor code is not
 * preempted by a multitasking OS except where it sleeps.  So every OS call
 * that can sleep (GEMDOS file, folder and memory calls, anything that waits)
 * made from the VDI must be bracketed:
 *
 *     VdiSwapState sw;
 *
 *     vdi_swap_pause(&sw);
 *     ... the OS call ...
 *     vdi_swap_resume(&sw);
 *
 * Pause puts the screen's variables back (a no-op when no bitmap is in
 * use); resume puts the bitmap's again.  The state is the caller's own, on
 * its stack, because the program that sleeps is not the only one that may
 * swap: a second program's call can swap and sleep, and the first resume
 * over the screen's variables as they are then.  Calls may nest only in
 * pairs; resume must be reached on every path.  With the NVDI extensions
 * off both are empty.
 */
#if CONF_WITH_VDI_NVDI_EXT
typedef struct {
    void *vwk;                  /* the bitmap workstation, or NULL: none was in use */
    WORD handle;
} VdiSwapState;

void vdi_swap_pause(VdiSwapState *state);
void vdi_swap_resume(VdiSwapState *state);
#else
typedef struct {
    WORD unused;
} VdiSwapState;

#define vdi_swap_pause(state)   ((void)(state))
#define vdi_swap_resume(state)  ((void)(state))
#endif

#endif /* VDI_SWAP_H */
