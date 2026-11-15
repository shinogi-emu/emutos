/*
 * vdi_ftos.h - file and memory services for the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * These functions are called by the outline font engine (compiled with
 * 32-bit int) and implemented by vdi_ftos.c using GEMDOS.  Like the
 * functions in vdi_ft2.h, they take and return only LONG, ULONG and
 * pointers.
 *
 * vdi_ftos.c is the OS glue: another host may build the VDI without it,
 * and supply these functions, ftmem_os_alloc() and ftmem_os_free() (see
 * vdi_ftmem.h) itself.  What a host's glue must do:
 *
 * - vdi_ft_alloc(), vdi_ft_free() and vdi_ft_realloc() take their blocks
 *   from vdi_ftmem.c (ftmem_alloc(), ftmem_free(), ftmem_realloc()),
 *   which refuses memory close to the bottom of the font stack (see
 *   VDI_FT_STACK_GUARD in vdi_ft2.h).  A host that does not use it must
 *   make that check itself, with outline_stack_low(), or deeply nested
 *   glyphs overflow the font stack.
 * - The memory and file functions, and ftmem_os_alloc() and
 *   ftmem_os_free(), are called on the font stack of vdi_outline.c, in
 *   EmuTOS's own memory.  An OS call that sleeps there must not wake up
 *   on it where the OS checks the stack pointer of a process (FreeMiNT
 *   kills a process whose stack pointer is below its own kernel stack
 *   when it wakes up): vdi_ftos.c makes its GEMDOS calls under MiNT or
 *   MagiC on the stack the VDI call came in on, below where that call
 *   stands (outline_caller_stack(), see vdi_defs.h).  Under a
 *   single-tasking GEMDOS nothing sleeps.
 * - What can sleep: the file and folder calls of vdi_font_file_load()
 *   and vdi_font_dir_scan(), and vdi_yield().  The memory calls behind
 *   ftmem_os_alloc() and ftmem_os_free() (Mxalloc() and Mfree() through
 *   vdi_kept.c here) stay on the font stack, and must not sleep: they do
 *   not under FreeMiNT or MagiC.
 * - Every OS call that can sleep is bracketed by vdi_sleep_begin() and
 *   vdi_sleep_end() (see vdi_sleep.h), and keeps nothing in statics.
 * - vdi_caller_is_system() and vdi_yield() are called off the font stack,
 *   by vdi_outline.c and vdi_nvdi.c (16-bit int code).
 */

#ifndef VDI_FTOS_H
#define VDI_FTOS_H

#include "portab.h"

void *vdi_ft_alloc(ULONG size);
void  vdi_ft_free(void *block);
void *vdi_ft_realloc(void *block, ULONG size);
void *vdi_font_file_load(const char *path, ULONG *size);  /* whole file, NULL on error */
void  vdi_font_file_free(void *data);
LONG  vdi_font_dir_scan(const char *dir,
                        void (*found)(const char *path, void *ctx), void *ctx);

/*
 * TRUE when the VDI is called by the operating system itself (under MiNT, a
 * process the kernel started: its parent is process 0), which must not get
 * a GDOS answer or start the font scan
 */
LONG  vdi_caller_is_system(void);

/*
 * let the other programs run for a moment (Syield() under MiNT or MagiC),
 * while the VDI waits for one of them to be done with the font engine;
 * FALSE, without waiting, if no other program can run
 */
LONG  vdi_yield(void);

#endif /* VDI_FTOS_H */
