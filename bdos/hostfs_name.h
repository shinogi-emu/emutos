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

#endif /* HOSTFS_NAME_H */
