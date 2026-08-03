/*
 * bdosstub.h - BDOS entry points, called by BIOS
 *
 * Copyright (C) 2019-2021 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef _BDOSSTUB_H
#define _BDOSSTUB_H

#include "bdosdefs.h"

/* BDOS initialization, called after BIOS initialization.
 * This is done in 2 parts: before and after Alt-RAM is available */
void osinit_before_xmaddalt(void);
void osinit_after_xmaddalt(void);

#if CONF_WITH_ALT_RAM
/* Register an Alt-RAM region to BDOS */
long xmaddalt(UBYTE *start, long size);

/* Get the total size of Alt-RAM regions */
long total_alt_ram(void);
#endif /* CONF_WITH_ALT_RAM */

/* BDOS quick pool.
 * Declared here because referenced by the BIOS OSHEADER */
#define MAXQUICK 5
extern WORD *root[MAXQUICK];

/* Pointer to the basepage of the current process.
 * Declared here because referenced by the BIOS OSHEADER,
 * and also by bios/kprint.c */
extern PD *run;

/* The handle for a character-device special name ("CON:", "AUX:",
 * "PRN:"), or 0 when the name is not one of them. Fopen()/Fcreate()
 * answer these from a table instead of from any filesystem, so a
 * caller deciding whether a name reaches a filesystem at all has to
 * consult the same table. */
LONG specname_handle(const char *name);

/* BDOS current date/time.
 * Declared here because also updated by XBIOS Settime() */
extern UWORD current_date, current_time;

#endif /* _BDOSSTUB_H */
