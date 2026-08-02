/*
 * qemuvirt.c - QEMU m68k "virt" machine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "qemuvirt.h"

#ifdef MACHINE_QEMU_VIRT

#define GF_TTY_REG(offset) (*(volatile ULONG *)(GF_TTY_BASE + (offset)))

/*
 * Write a single character to the goldfish-tty.
 *
 * No initialisation is needed: QEMU forwards the byte to the chardev
 * synchronously, so there is no ready flag to poll and nothing can be
 * lost.  That makes this usable from the very first instructions of
 * the boot, which is the whole point of having it.
 */
void qemuvirt_kprintf_outc(int c)
{
    if (c == '\n')
        GF_TTY_REG(GF_TTY_PUT_CHAR) = '\r';

    GF_TTY_REG(GF_TTY_PUT_CHAR) = (ULONG)(UBYTE)c;
}

#endif /* MACHINE_QEMU_VIRT */
