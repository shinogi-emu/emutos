/*
 * hostfs_link.h - host folder transport over virtio-serial
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef HOSTFS_LINK_H
#define HOSTFS_LINK_H

#include "portab.h"

/*
 * PATHS ON THE WIRE
 *
 * Every `path` below is the wire spelling defined by hostfs_proto.h:
 * drive-relative, '/' as its only separator, no leading '/', and made
 * of the HOST's own names rather than their 8.3 clippings. The empty
 * string is the drive root. The GEMDOS layer builds them; nothing here
 * rewrites a path, so there is exactly one place where a guest path
 * becomes a host one.
 *
 * Every call returns E_OK or a positive count on success and a negative
 * GEMDOS error otherwise -- never an errno, and never a host status.
 * Once the channel has lost sync every later call is EDRIVE; see the
 * note on hl_kill() in hostfs_link.c.
 */

/*
 * These restate hostfs_proto.h for callers that must NOT include it.
 *
 * bdos/hostfs.h has its own HOSTFS_MAX_PATH and HOSTFS_MAX_NAME, sized
 * by what a GEMDOS caller's buffer can hold, and they are much smaller
 * than the wire's.  Including both headers in one file would silently
 * redefine one pair with the other.  hostfs_link.c includes the wire
 * header and asserts at compile time that these agree with it, so the
 * two cannot drift apart unnoticed.
 */
#define HOSTFS_LINK_O_RDONLY    0
#define HOSTFS_LINK_O_WRONLY    1
#define HOSTFS_LINK_O_RDWR      2

#define HOSTFS_LINK_MAX_PATH    1024

void hostfs_link_attach(WORD slot);
WORD hostfs_link_present(void);

/* Attributes are decomposed the way the GEMDOS layer wants them. */
LONG hostfs_link_stat(const char *path, ULONG *size, UWORD *is_dir,
                      ULONG *mtime, UWORD *writable);

/* mode is HOSTFS_O_RDONLY / _WRONLY / _RDWR.  Returns a host handle. */
LONG hostfs_link_open(const char *path, UWORD mode);
LONG hostfs_link_close(ULONG handle);

/* Returns the byte count actually read, chunking internally. */
LONG hostfs_link_read(ULONG handle, ULONG offset, UBYTE *buf, ULONG count);

LONG hostfs_link_opendir(const char *path);

/*
 * ONE entry per call, plus the cookie for the next one.  Pass 0 to
 * start.  ENMFIL ends the enumeration.  A name too long for NAMEMAX is
 * reported as an empty name with the cookie still advanced, so the
 * caller can count what it could not show instead of losing the rest of
 * the directory.
 */
LONG hostfs_link_readdir(ULONG handle, ULONG cookie, char *name,
                         ULONG namemax, ULONG *next);
LONG hostfs_link_closedir(ULONG handle);

#endif /* HOSTFS_LINK_H */
