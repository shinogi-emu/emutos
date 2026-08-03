/*
 * hostfs_name.h - 8.3 name mapping for the host-folder drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef HOSTFS_NAME_H
#define HOSTFS_NAME_H

/* Clip a host name to a NUL-terminated 8.3 name. out >= 13 bytes. */
void hostfs_host_to_atari(const char *host, char *out);

/* Clip an Atari name for host lookup: FIRST dot, verbatim. out >= 13. */
void hostfs_atari_clip(const char *name, char *out);

/* Byte-order comparison on raw host names. strcmp semantics. */
int hostfs_name_compare(const char *a, const char *b);

#endif /* HOSTFS_NAME_H */
