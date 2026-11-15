/*
 * vdi_kept.h - memory that outlives the program making the VDI call
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_KEPT_H
#define VDI_KEPT_H

#include "portab.h"

/*
 * OS glue: another host may build the VDI without this file and supply
 * these functions.
 */
BOOL vdi_other_gemdos(void);
void *vdi_alloc_kept(LONG size);
void *vdi_alloc_kept_blit(LONG size);   /* in ST-RAM if a blitter has to reach it */
void vdi_free_kept(void *block);
void *vdi_alloc_readable(LONG size);    /* every program can read it */

/*
 * A block that outlives its owner, with the Mxalloc() mode (RAM kind and
 * protection bits) given: the workstations (Vwk) and the pixels of the VDI's
 * bitmaps.  They are kept, not owned by the calling program: a block may be
 * freed from another program's call, after the program that opened it has
 * ended (a closed workstation waits while any program sleeps in an OS call,
 * see vdi_sleep.h).  A program that ends without closing its workstation or
 * bitmaps leaks the blocks.  A host with its own allocator supplies this.
 */
void *vdi_alloc_kept_mode(LONG size, WORD mode);

/*
 * GEMDOS Mxalloc() and Mfree() with a trap of their own, bracketed as
 * vdi_sleep.h says.  vdi_os_mfree() returns the result of Mfree(): 0 if done.
 */
LONG vdi_os_mxalloc(LONG size, WORD mode);
LONG vdi_os_mfree(void *block);

#endif /* VDI_KEPT_H */
