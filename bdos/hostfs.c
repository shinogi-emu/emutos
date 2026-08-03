/*
 * hostfs.c - host folder as a GEMDOS drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The drive is claimed one GEMDOS call at a time rather than through a
 * filesystem abstraction, because EmuTOS has no such abstraction and
 * inventing one would mean threading it through the whole FAT path.
 * Anything not claimed here falls through to stock EmuTOS untouched.
 */

#include "emutos.h"
#include "hostfs.h"
#include "hostfs_name.h"
#include "../bios/virtio_9p.h"
#include "tosvars.h"
#include "kprint.h"
#include "gemerror.h"

#ifdef MACHINE_QEMU_VIRT

static WORD hostfs_on;

void hostfs_init(void)
{
    if (!virtio_9p_present())
        return;

    drvbits |= (1L << HOSTFS_DRIVE);
    hostfs_on = 1;

    KINFO(("hostfs: drive C registered\n"));
}

WORD hostfs_claims(WORD fn, short *pw)
{
    (void)fn;
    (void)pw;

    if (!hostfs_on)
        return 0;

    return 0;       /* call surface lands in Task 7 */
}

LONG hostfs_dispatch(WORD fn, short *pw)
{
    (void)fn;
    (void)pw;

    return EINVFN;
}

#endif /* MACHINE_QEMU_VIRT */
