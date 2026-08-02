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
#include "vectors.h"

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
 * 200 Hz tick
 *
 * The goldfish-rtc alarm is one-shot: firing it clears alarm_running and
 * nothing re-arms it (hw/rtc/goldfish_rtc.c).  There is no other timer
 * on this machine, so the periodic tick is rebuilt by re-arming inside
 * every interrupt.
 *
 * The next deadline is computed from the previous deadline rather than
 * from "now", so the tick cannot drift by the handler's own latency.
 * If we ever fall far enough behind that the new deadline is already
 * past, the catch-up is bounded -- QEMU fires an alarm set in the past
 * synchronously from the register write, so an unbounded loop here would
 * become an interrupt storm.
 *
 * The counter is 64-bit nanoseconds.  It is kept as two longwords
 * because EmuTOS builds with -mshort and has no 64-bit type.
 */

#define GF_RTC_REG(n, offset) (*(volatile ULONG *)(GF_RTC(n) + (offset)))
#define GF_PIC_REG(n, offset) (*(volatile ULONG *)(GF_PIC(n) + (offset)))

/* Next deadline per RTC, as two longwords, and the period each runs at. */
static ULONG rtc_next_hi[GF_RTC_NB];
static ULONG rtc_next_lo[GF_RTC_NB];
static ULONG rtc_period[GF_RTC_NB];

/*
 * Diagnostics.  rtc_skipped counts individual ticks walked past during
 * bounded catch-up; rtc_resync counts the times the gap was so large we
 * gave up and jumped the deadline to the present.  A non-zero resync
 * count means the guest clock is behind real time and cannot be trusted.
 */
ULONG rtc_skipped[GF_RTC_NB];
ULONG rtc_resync[GF_RTC_NB];

/* Read the counter.  TIME_LOW must be read first: it latches TIME_HIGH. */
static void rtc_now(WORD n, ULONG *hi, ULONG *lo)
{
    *lo = GF_RTC_REG(n, GF_RTC_TIME_LOW);
    *hi = GF_RTC_REG(n, GF_RTC_TIME_HIGH);
}

/* Arm the alarm.  ALARM_HIGH only stores; writing ALARM_LOW is what arms. */
static void rtc_arm(WORD n)
{
    GF_RTC_REG(n, GF_RTC_ALARM_HIGH) = rtc_next_hi[n];
    GF_RTC_REG(n, GF_RTC_ALARM_LOW) = rtc_next_lo[n];
}

/* TRUE if the deadline in (hi,lo) is at or before (nhi,nlo). */
static WORD rtc_reached(ULONG hi, ULONG lo, ULONG nhi, ULONG nlo)
{
    if (hi != nhi)
        return (hi < nhi);
    return (lo <= nlo);
}

/*
 * Push a deadline forward by its period, skipping any already missed.
 *
 * Bounded deliberately.  Arming an alarm that is already in the past
 * makes QEMU raise the interrupt synchronously from the ALARM_LOW write
 * (goldfish_rtc_set_alarm), so an unbounded "advance until future" loop
 * would turn a late tick into an interrupt storm rather than recover
 * from it.
 */
static void rtc_advance(WORD n)
{
    ULONG nhi, nlo;
    WORD guard;

    rtc_now(n, &nhi, &nlo);

    for (guard = 0; guard < QEMUVIRT_CATCHUP_MAX; guard++)
    {
        ULONG prev = rtc_next_lo[n];

        rtc_next_lo[n] += rtc_period[n];
        if (rtc_next_lo[n] < prev)      /* carry into the high longword */
            rtc_next_hi[n]++;

        if (!rtc_reached(rtc_next_hi[n], rtc_next_lo[n], nhi, nlo))
        {
            rtc_skipped[n] += guard;    /* 0 in the normal case */
            return;
        }
    }

    /*
     * Too far behind to walk forward tick by tick.  This is not a late
     * handler: it is the host having been suspended, or the VM stopped
     * in the debugger, where the gap can be hours -- millions of ticks.
     *
     * Resync the deadline to now rather than grinding through them.
     * That deliberately drops the missed ticks, so _hz_200 runs slow by
     * exactly the outage: the alternative is an interrupt storm lasting
     * as long as the outage did.  Record it, because the visible symptom
     * of dropped ticks is a wrong clock and wrong timeouts much later,
     * which looks nothing like a timer fault.
     */
    rtc_resync[n]++;
    rtc_next_hi[n] = nhi;
    rtc_next_lo[n] = nlo;

    rtc_next_lo[n] += rtc_period[n];
    if (rtc_next_lo[n] < nlo)
        rtc_next_hi[n]++;
}

/* Silence one RTC, then re-arm it for the next period. */
static void rtc_reload(WORD n)
{
    /*
     * CLEAR_INTERRUPT is the write that actually deasserts the line --
     * it is the only one that clears irq_pending, and the line follows
     * irq_pending & irq_enabled.  CLEAR_ALARM cancels the one-shot,
     * which is a no-op here since it has already fired, but keeps the
     * device in a defined state.
     */
    GF_RTC_REG(n, GF_RTC_CLEAR_ALARM) = 1;
    GF_RTC_REG(n, GF_RTC_CLEAR_INTERRUPT) = 1;

    rtc_advance(n);
    rtc_arm(n);
}

/*
 * Acknowledge whatever fired, and say what it was.
 *
 * Called from the level 6 assembler stub, which dispatches on the
 * result.  Both RTCs share autovector 6, so the PIC's pending register
 * decides which of them fired -- and both may have fired at once.  The
 * PIC has no acknowledge register of its own and is level-driven, so
 * silencing the devices is what lowers the level; miss that and we
 * re-enter immediately on rte.
 *
 * This returns rather than calling the EmuTOS handlers itself, because
 * those are assembler routines with their own entry contract.
 */
WORD qemuvirt_timer_ack(void)
{
    ULONG pending = GF_PIC_REG(GF_RTC_PIC, GF_PIC_IRQ_PENDING);
    WORD fired = 0;

    if (pending & GF_RTC_BIT(QEMUVIRT_RTC_TICK))
    {
        rtc_reload(QEMUVIRT_RTC_TICK);
        fired |= QEMUVIRT_FIRED_TICK;
    }

    if (pending & GF_RTC_BIT(QEMUVIRT_RTC_VBL))
    {
        rtc_reload(QEMUVIRT_RTC_VBL);
        fired |= QEMUVIRT_FIRED_VBL;
    }

    return fired;
}

/*
 * Bring one RTC up at the given period, and unmask its PIC line.
 *
 * The offset staggers this timer's phase against the others.  Without
 * it the 200 Hz tick and the 50 Hz VBL, both starting from the same
 * instant, coincide on every fourth tick -- and since the VBL body is
 * much longer than the tick body, that would make every fourth service
 * window a latency spike.  Real hardware derives the two from separate
 * oscillators and collides only occasionally; staggering deliberately
 * is closer to that in effect, if not in mechanism.
 */
static void rtc_start(WORD n, ULONG period, ULONG offset)
{
    ULONG nhi, nlo;

    GF_RTC_REG(n, GF_RTC_IRQ_ENABLED) = 0;
    GF_RTC_REG(n, GF_RTC_CLEAR_ALARM) = 1;
    GF_RTC_REG(n, GF_RTC_CLEAR_INTERRUPT) = 1;

    rtc_period[n] = period;
    rtc_now(n, &nhi, &nlo);
    rtc_next_hi[n] = nhi;
    rtc_next_lo[n] = nlo + offset;
    if (rtc_next_lo[n] < nlo)
        rtc_next_hi[n]++;
    rtc_advance(n);

    /* Every PIC line comes up masked after reset; unmask just this one. */
    GF_PIC_REG(GF_RTC_PIC, GF_PIC_ENABLE) = GF_RTC_BIT(n);

    GF_RTC_REG(n, GF_RTC_IRQ_ENABLED) = 1;
    rtc_arm(n);
}

/*
 * Install the level 6 vector and start both timers.
 *
 * Called from init_system_timer(), which runs before bios_init() lowers
 * the IPL mask, so nothing can arrive until we are ready.  The vector
 * goes in before the PIC lines are unmasked; vecs_init() has already
 * pointed every autovector at just_rte, so a stray line in between is
 * absorbed rather than fatal.
 */
void qemuvirt_init_system_timer(void)
{
    VEC_LEVEL6 = qemuvirt_timer_int;

    rtc_start(QEMUVIRT_RTC_TICK, QEMUVIRT_TICK_NS, 0);
    rtc_start(QEMUVIRT_RTC_VBL, QEMUVIRT_VBL_NS, QEMUVIRT_TICK_NS / 2);
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
