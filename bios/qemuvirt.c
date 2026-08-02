/*
 * qemuvirt.c - QEMU m68k "virt" machine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "emutos.h"
#include "bios.h"
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
void kprintf_outc_qemuvirt(int c)
{
    if (c == '\n')
        GF_TTY_REG(GF_TTY_PUT_CHAR) = '\r';

    GF_TTY_REG(GF_TTY_PUT_CHAR) = (ULONG)(UBYTE)c;
}

/*
 * Bootinfo
 *
 * QEMU writes a list of tagged records immediately after the loaded
 * image, at what it calls parameters_base.  That address is not passed
 * to the guest in any register: it is
 *
 *     (highest loaded address + 1) & ~1
 *
 * where the highest loaded address spans BSS, because it is derived
 * from p_memsz rather than p_filesz.  See hw/m68k/virt.c:239 and
 * include/hw/elf_ops.h.inc:578.  For our link script that address is
 * the end of the BSS segment rounded up to a word boundary.
 *
 * Note this is _ebss and not __end_os_stram: the latter lives in the
 * ALIGN(4) .laststram section and can sit a few bytes higher, which
 * would step straight over the first record.
 *
 * These routines are called from memconf, before BSS has been cleared,
 * so they must not touch any global state.  They re-walk the list on
 * each call instead of caching, which costs nothing at boot.
 *
 * All fields are big-endian.  Accessors are byte-wise so that the
 * 2-byte alignment of parameters_base cannot produce a misaligned
 * longword read.
 */

static UWORD bi_getword(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | (UWORD)p[1]);
}

static ULONG bi_getlong(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16)
         | ((ULONG)p[2] << 8)  | ((ULONG)p[3]);
}

/*
 * Find a record by tag.  Returns a pointer to its payload, or NULL.
 *
 * The list is validated before it is trusted: the first record must be
 * BI_MACHTYPE carrying MACH_VIRT.  Anything else means we are not
 * looking at a bootinfo list at all, and the caller must fall back
 * rather than act on whatever happens to be in memory.
 */
static const UBYTE *bi_find(UWORD wanted)
{
    const UBYTE *p = (const UBYTE *)((((ULONG)_ebss) + 1) & ~1UL);
    WORD guard;

    if (bi_getword(p) != BI_MACHTYPE)
        return NULL;
    if (bi_getlong(p + 4) != MACH_VIRT)
        return NULL;

    /* The list is short; the guard only stops a corrupt one running away. */
    for (guard = 0; guard < 64; guard++) {
        UWORD tag = bi_getword(p);
        UWORD size = bi_getword(p + 2);

        if (tag == BI_LAST)
            return NULL;
        if (size < 4)               /* malformed: would not advance */
            return NULL;
        if (tag == wanted)
            return p + 4;

        p += size;
    }

    return NULL;
}

/*
 * Total RAM according to the bootinfo, or 0 if it could not be read.
 *
 * BI_MEMCHUNK carries a base and a size.  On this machine the base is
 * always 0 (hw/m68k/virt.c:257 passes 0 and ram_size), but the base is
 * honoured rather than assumed.
 */
static ULONG qemuvirt_ramsize(void)
{
    const UBYTE *p = bi_find(BI_MEMCHUNK);
    ULONG total;

    if (p == NULL)
        return 0;

    total = bi_getlong(p) + bi_getlong(p + 4);

    /*
     * QEMU permits close to 4GB here.  BDOS describes a memory block
     * with a signed LONG length (see themd.m_length in biosmem.c), so
     * anything at or above 2GB would present as negative and produce
     * silent nonsense in the allocator.  Clamp rather than trust it.
     */
    if (total > QEMUVIRT_MAX_RAM)
        total = QEMUVIRT_MAX_RAM;

    return total;
}

/*
 * Cached RAM size.
 *
 * The bootinfo cannot be read late.  It sits immediately past our BSS,
 * which is exactly where membot starts, so the first OS allocation
 * overwrites it -- measured at two bytes past the first record.  The
 * value is therefore captured once, early, and everything afterwards
 * uses the copy.
 *
 * This lives in BSS, so it may only be filled in after BSS has been
 * cleared, i.e. from machine_init() onwards -- not from the memconf
 * path, which runs earlier and re-reads the list directly.
 */
static ULONG cached_ramsize;

void qemuvirt_machine_init(void)
{
    cached_ramsize = qemuvirt_ramsize();
}

/*
 * End of ST-RAM, for _phystop.
 *
 * Called from the RAM-resident arm of memchk in memory.S, before BSS is
 * cleared, so this one re-reads the bootinfo rather than using the
 * cache.  ST-RAM is capped at QEMUVIRT_MAX_STRAM; anything above that
 * is offered separately as Alt-RAM.
 */
ULONG qemuvirt_memconf(void)
{
    ULONG total = qemuvirt_ramsize();

    if (total == 0)
        return QEMUVIRT_FALLBACK_STRAM;

    if (total > QEMUVIRT_MAX_STRAM)
        return QEMUVIRT_MAX_STRAM;

    return total;
}

/*
 * Base of the Alt-RAM block, or 0 if there is none.
 *
 * Deliberately the ST-RAM cap and not memtop: xmaddalt() only checks
 * overlap against memtop, which has already been lowered by the screen
 * allocation, so a block based on memtop would silently alias the
 * framebuffer.
 */
ULONG qemuvirt_altram_base(void)
{
    return (cached_ramsize > QEMUVIRT_MAX_STRAM) ? QEMUVIRT_MAX_STRAM : 0;
}

/* Size of the Alt-RAM block, or 0 if there is none. */
ULONG qemuvirt_altram_size(void)
{
    return (cached_ramsize > QEMUVIRT_MAX_STRAM)
         ? (cached_ramsize - QEMUVIRT_MAX_STRAM) : 0;
}

/*
 * CPU type, from BI_CPUTYPE.
 *
 * QEMU derives this from -cpu (hw/m68k/virt.c:243-256), which is the
 * only reliable source here: the usual PCR probe cannot be used because
 * QEMU aborts the machine on an unimplemented control register rather
 * than raising an exception, for every CPU model.  Returns the EmuTOS
 * convention (20, 30, 40, 60), defaulting to 40 -- the machine default.
 */
WORD qemuvirt_cputype(void)
{
    const UBYTE *p = bi_find(BI_CPUTYPE);
    ULONG cpu;

    if (p == NULL)
        return 40;

    cpu = bi_getlong(p);

    if (cpu & CPU_68060)
        return 60;
    if (cpu & CPU_68040)
        return 40;
    if (cpu & CPU_68030)
        return 30;
    if (cpu & CPU_68020)
        return 20;

    return 40;
}

#endif /* MACHINE_QEMU_VIRT */
