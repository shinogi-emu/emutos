/*
 * qemuvirt.h - QEMU m68k "virt" machine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The "virt" machine is a synthetic platform provided by QEMU for m68k.
 * It has no Atari hardware at all: peripherals are a set of Goldfish
 * devices plus 128 virtio-mmio transports.  See hw/m68k/virt.c in the
 * QEMU sources for the authoritative memory map.
 */

#ifndef QEMUVIRT_H
#define QEMUVIRT_H

#ifdef MACHINE_QEMU_VIRT

/*
 * goldfish-tty (QEMU: hw/char/goldfish_tty.c)
 *
 * Registers are longword-only: the device declares
 * impl.min_access_size = impl.max_access_size = 4, so byte or word
 * accesses do not reach it as written.  The register window is
 * DEVICE_NATIVE_ENDIAN, i.e. big-endian for an m68k target, so no
 * byte swapping is required here.  (This is *not* true of the
 * virtio-mmio transports, whose registers are little-endian.)
 */
#define GF_TTY_BASE         0xff008000UL

#define GF_TTY_PUT_CHAR     0x00    /* W: emit the low byte           */
#define GF_TTY_BYTES_READY  0x04    /* R: bytes in the receive FIFO   */
#define GF_TTY_CMD          0x08    /* W: see GF_TTY_CMD_* below      */
#define GF_TTY_VERSION      0x20    /* R: always 1                    */

#define GF_TTY_CMD_INT_DISABLE  0
#define GF_TTY_CMD_INT_ENABLE   1

void qemuvirt_kprintf_outc(int c);

#endif /* MACHINE_QEMU_VIRT */

#endif /* QEMUVIRT_H */
