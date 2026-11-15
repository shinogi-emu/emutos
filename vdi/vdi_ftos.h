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

#endif /* VDI_FTOS_H */
