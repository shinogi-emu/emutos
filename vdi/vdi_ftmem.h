/*
 * vdi_ftmem.h - the memory allocator of the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VDI_FTMEM_H
#define VDI_FTMEM_H

#include "portab.h"

/*
 * Blocks for the outline font engine, aligned to 8 bytes.  Small ones are
 * carved from large arenas, so that the engine's many small blocks do not
 * each take a GEMDOS memory descriptor.
 */
void *ftmem_alloc(ULONG size);
void ftmem_free(void *block);
void *ftmem_realloc(void *block, ULONG size);   /* the old block stays on failure */

/*
 * What the allocator needs from the system, provided by vdi_ftos.c:
 * blocks that live until they are freed, whichever program is running.
 * They are asked for on the font stack, and must not sleep.
 */
void *ftmem_os_alloc(ULONG size);
void ftmem_os_free(void *block);

/*
 * What it needs from the VDI, provided by vdi_outline.c: TRUE when the
 * call on the font stack has come within VDI_FT_STACK_GUARD bytes of its
 * bottom (see vdi_ft2.h), where ftmem_alloc() gives nothing
 */
BOOL outline_stack_low(void);

#endif /* VDI_FTMEM_H */
