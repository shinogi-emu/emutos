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
 * GEMDOS Mxalloc() and Mfree() with a trap of their own, bracketed as
 * vdi_sleep.h says: for the workstations and bitmaps, which belong to
 * the calling program
 */
LONG vdi_os_mxalloc(LONG size, WORD mode);
void vdi_os_mfree(void *block);

#endif /* VDI_KEPT_H */
