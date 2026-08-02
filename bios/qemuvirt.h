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
 * goldfish-pic (QEMU: hw/intc/goldfish_pic.c)
 *
 * Six instances, 0x1000 apart.  Each aggregates 32 input lines into one
 * 68k interrupt level: PIC #N drives IPL N and autovector 24+N.
 *
 * Registers are DEVICE_NATIVE_ENDIAN and longword-only, as for the TTY.
 * There is no acknowledge register: the PIC is level-driven off the
 * device's own line, so a handler must silence the device itself or the
 * level stays asserted and the CPU re-enters immediately.
 */
#define GF_PIC_BASE         0xff000000UL
#define GF_PIC_STRIDE       0x1000UL
#define GF_PIC_NB           6

/* PIC #N (1-based), as numbered in QEMU's own comments */
#define GF_PIC(n)           (GF_PIC_BASE + ((ULONG)((n) - 1) * GF_PIC_STRIDE))

#define GF_PIC_STATUS       0x00    /* R: popcount(pending & enabled)  */
#define GF_PIC_IRQ_PENDING  0x04    /* R: pending & enabled, as a mask */
#define GF_PIC_DISABLE_ALL  0x08    /* W: clear enabled and pending    */
#define GF_PIC_DISABLE      0x0c    /* W: enabled &= ~value            */
#define GF_PIC_ENABLE       0x10    /* W: enabled |= value             */

/*
 * goldfish-rtc (QEMU: hw/rtc/goldfish_rtc.c)
 *
 * Two instances, 0x1000 apart.  virt.c constructs them with
 * big-endian=true, so unlike virtio-mmio these registers need no byte
 * swapping on m68k.
 *
 * The counter is NANOSECONDS since the Unix epoch, and the alarm is
 * ONE-SHOT -- nothing re-arms it.  A periodic tick has to be rebuilt by
 * the handler on every interrupt.
 *
 * RTC 0 raises flat IRQ 168, which is PIC #6 bit 0, i.e. IPL 6.
 */
#define GF_RTC_BASE         0xff006000UL
#define GF_RTC_STRIDE       0x1000UL
#define GF_RTC_NB           2

/* RTC #n, 0-based */
#define GF_RTC(n)           (GF_RTC_BASE + ((ULONG)(n) * GF_RTC_STRIDE))

#define GF_RTC_TIME_LOW         0x00    /* R: low 32 bits, latches high */
#define GF_RTC_TIME_HIGH        0x04    /* R: high 32, read second      */
#define GF_RTC_ALARM_LOW        0x08    /* W: arms the alarm            */
#define GF_RTC_ALARM_HIGH       0x0c    /* W: does NOT arm              */
#define GF_RTC_IRQ_ENABLED      0x10    /* RW: bit 0                    */
#define GF_RTC_CLEAR_ALARM      0x14    /* W: cancel                    */
#define GF_RTC_ALARM_STATUS     0x18    /* R: 1 while armed             */
#define GF_RTC_CLEAR_INTERRUPT  0x1c    /* W: deassert the line         */

/*
 * Both RTCs hang off PIC #6: RTC 0 on IRQ #1 (bit 0), RTC 1 on IRQ #2
 * (bit 1).  Having two independently armed one-shots means the 200 Hz
 * system tick and the VBL do not have to be divided out of a single
 * source -- each keeps its own deadline.
 *
 * Because both share one autovector, the level 6 handler must read the
 * PIC's pending register to find out which of them fired.
 */
#define GF_RTC_PIC          6       /* PIC carrying both RTC lines */
#define GF_RTC_IPL          6       /* resulting 68k level         */

#define QEMUVIRT_RTC_TICK   0       /* RTC index driving _hz_200   */
#define QEMUVIRT_RTC_VBL    1       /* RTC index driving the VBL   */

#define GF_RTC_BIT(n)       (1UL << (n))    /* PIC bit for RTC #n  */

/*
 * Display geometry. The VDI renders 16bpp chunky here and the result is
 * converted to ARGB for virtio-gpu, because virtio-gpu has no 16-bit
 * format and EmuTOS has no 32-bit rendering path.
 *
 * Width must be a multiple of 8: update_rez_dependent() computes the
 * stride as V_REZ_HZ / 8 * v_planes.
 */
#define QEMUVIRT_SCREEN_W   1280
#define QEMUVIRT_SCREEN_H   720

/*
 * The ARGB surface virtio-gpu scans out, and the colour table used to
 * produce it, both live in Alt-RAM. They must not come from ST-RAM: the
 * VDI framebuffer is allocated at the top of ST-RAM by balloc_stram(),
 * and a fixed ST-RAM address large enough for the ARGB surface would
 * run into it.
 *
 * The table maps every possible 16-bit VDI pixel to a finished ARGB
 * longword, so the per-pixel work is one indexed load. Expansion,
 * target byte order and the unused bit of RRRRRGGGGG0BBBBB all resolve
 * at table-build time and disappear from the conversion loop.
 */
#define QEMUVIRT_ARGB_FB    0x01000000UL  /* 16MB: W*H*4 = 3.5MB      */
#define QEMUVIRT_CTAB       0x01400000UL  /* 20MB: 65536 * 4 = 256KB  */
#define QEMUVIRT_CTAB_SIZE  65536UL

/* Present every Nth VBL until damage tracking makes that unnecessary. */
#define QEMUVIRT_PRESENT_DIV    5

/* 200 Hz in nanoseconds */
#define QEMUVIRT_TICK_NS    5000000UL

/* VBL rate.  50 Hz matches the PAL default EmuTOS builds for. */
#define QEMUVIRT_VBL_NS     20000000UL

/*
 * How many missed ticks we will walk forward through before giving up
 * and resyncing the deadline to the present.  Sized for a genuinely
 * late handler, not for a suspended host: an hour of laptop sleep is
 * 720000 missed ticks, and walking those would be an interrupt storm
 * lasting as long as the outage.
 */
#define QEMUVIRT_CATCHUP_MAX    64

extern ULONG rtc_skipped[];     /* ticks walked past during catch-up */
extern ULONG rtc_resync[];      /* times the deadline was jumped     */

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
void qemuvirt_init_system_timer(void);
void qemuvirt_timer_int(void);   /* asm, bios/qemuvirt2.S */

/* Bits returned by qemuvirt_timer_ack() */
#define QEMUVIRT_FIRED_TICK 0x01
#define QEMUVIRT_FIRED_VBL  0x02

WORD qemuvirt_timer_ack(void);
ULONG qemuvirt_memconf(void);
ULONG qemuvirt_altram_base(void);
ULONG qemuvirt_altram_size(void);
WORD qemuvirt_cputype(void);

#endif /* MACHINE_QEMU_VIRT */

#endif /* QEMUVIRT_H */
