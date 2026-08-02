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

/*
 * Bootinfo (QEMU: hw/m68k/bootinfo.h, standard-headers/asm-m68k/bootinfo.h)
 *
 * QEMU deposits a list of tagged records immediately after the loaded
 * image.  All fields are big-endian, and each record's size covers the
 * whole record including its 4-byte header, so the list is walked by
 * adding size -- record payloads are not all the same length.
 */
#define BI_LAST         0x0000  /* sentinel, no payload                  */
#define BI_MACHTYPE     0x0001  /* ULONG machine type                    */
#define BI_CPUTYPE      0x0002  /* ULONG cpu type                        */
#define BI_MEMCHUNK     0x0005  /* ULONG base, ULONG size                */

#define MACH_VIRT       14      /* BI_MACHTYPE value for this machine    */

#define CPU_68020       (1<<0)
#define CPU_68030       (1<<1)
#define CPU_68040       (1<<2)
#define CPU_68060       (1<<3)

/*
 * Largest ST-RAM we will claim, regardless of how much -m gave us.
 * This matches the 14M stram region in emutos.ld.  Anything above it
 * needs to be surfaced as Alt-RAM instead.
 */
#define QEMUVIRT_MAX_STRAM  (14UL * 1024 * 1024)

/* Fallback if the bootinfo cannot be validated */
#define QEMUVIRT_FALLBACK_STRAM  (4UL * 1024 * 1024)

/*
 * Most RAM we will describe, whatever -m says.  BDOS records a memory
 * block length in a signed LONG, so 2GB and above would read as
 * negative.  QEMU itself allows nearly 4GB.
 */
#define QEMUVIRT_MAX_RAM    (2032UL * 1024 * 1024)

void kprintf_outc_qemuvirt(int c);
void qemuvirt_machine_init(void);
ULONG qemuvirt_memconf(void);
ULONG qemuvirt_altram_base(void);
ULONG qemuvirt_altram_size(void);
WORD qemuvirt_cputype(void);

#endif /* MACHINE_QEMU_VIRT */

#endif /* QEMUVIRT_H */
