/*
 * virtio.c - virtio-mmio transport
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#define ENABLE_KDEBUG

#include "emutos.h"
#include "virtio.h"
#include "kprint.h"
#include "virtio_gpu.h"
#include "qemuvirt.h"
#include "virtio_input.h"
#include "virtio_9p.h"
#include "virtio_blk.h"
#include "hostfs_link.h"

#ifdef MACHINE_QEMU_VIRT

#define VIO_REG(slot, reg) \
    (*(volatile ULONG *)(VIRTIO_SLOT(slot) + (ULONG)(reg)))

/*
 * Byte swap.  Written out rather than borrowed from anywhere so that
 * there is exactly one place to look when a value comes back wrong.
 */
static ULONG bswap32(ULONG v)
{
    return ((v & 0x000000ffUL) << 24)
         | ((v & 0x0000ff00UL) << 8)
         | ((v & 0x00ff0000UL) >> 8)
         | ((v & 0xff000000UL) >> 24);
}

/*
 * Register access.
 *
 * The modern virtio-mmio register window is DEVICE_LITTLE_ENDIAN, so
 * QEMU's memory core swaps every 32-bit access on this big-endian
 * target.  These undo that.  Nothing else in the driver may touch a
 * transport register directly.
 */
ULONG vio_read32(WORD slot, WORD reg)
{
    return bswap32(VIO_REG(slot, reg));
}

void vio_write32(WORD slot, WORD reg, ULONG value)
{
    VIO_REG(slot, reg) = bswap32(value);
}

/*
 * Ring field access.
 *
 * Every ring field is little-endian for a VERSION_1 device, by a
 * completely different mechanism from the register swap: rings are DMA
 * through guest RAM and QEMU converts them with virtio_tswap*, while
 * registers are swapped by the memory core. Each needs exactly one
 * swap from us; neither cancels the other.
 *
 * Ring words are aligned by virtq_init(). Indices must use a single
 * word access: two byte stores can expose a backwards index at a carry.
 * Wider descriptor fields remain byte-wise and become visible only
 * after the available index has been published.
 */
static void st16le(UBYTE *p, UWORD v)
{
    *(volatile UWORD *)p = (UWORD)((v >> 8) | (v << 8));
}

static UWORD ld16le(const UBYTE *p)
{
    UWORD v = *(const volatile UWORD *)p;
    return (UWORD)((v >> 8) | (v << 8));
}

static void st32le(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)((v >> 8) & 0xff);
    p[2] = (UBYTE)((v >> 16) & 0xff);
    p[3] = (UBYTE)((v >> 24) & 0xff);
}

static ULONG ld32le(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* 64-bit descriptor address; we are always below 4GB so the high half is 0. */
static void st64le(UBYTE *p, ULONG v)
{
    st32le(p, v);
    st32le(p + 4, 0);
}

/*
 * Bring one queue up.
 *
 * Order is not negotiable. QUEUE_NUM must be written before
 * QUEUE_READY: if it is missing, virtio_queue_set_num() rejects the
 * zero and the device silently keeps its own default size while we
 * index the rings with ours. Likewise QUEUE_READY with the addresses
 * still zero leaves the region cache NULL and every later kick is
 * silently dropped. Neither mistake produces a diagnostic.
 *
 * Returns 0 on success.
 */
WORD virtq_setup(VIRTQ *vq, WORD slot, WORD index, WORD num, UBYTE *mem)
{
    ULONG max;
    WORD i;
    ULONG avail_off, used_off;

    vq->slot = slot;
    vq->index = index;
    vq->avail_idx = 0;
    vq->last_used = 0;

    /*
     * num is published at the END of this function, not here.
     *
     * It is the only thing that says a queue is real. vi_drain() runs off
     * the 200 Hz tick and tests it before touching the used ring, and that
     * interrupt is live while this runs: virtio_input_init() sets DRIVER_OK
     * on the keyboard before it starts the tablet. Setting num on entry
     * declared the queue usable some forty lines before desc/avail/used were
     * assigned, so a tick landing in the gap read a used pointer that was
     * still NULL, took ld16(NULL+2) out of the vector table, and walked a
     * ring that did not exist -- dispatching whatever it found there as key
     * and mouse events, from inside the interrupt. The gap spans two MMIO
     * reads and a memset of over a kilobyte, which is why it hung the boot
     * often enough to be noticed and not often enough to be reliable.
     *
     * Clearing it here also covers the two failure returns below, which
     * otherwise left a nonzero num beside a stale used pointer.
     */
    vq->num = 0;

    vio_write32(slot, VIRTIO_MMIO_QUEUE_SEL, (ULONG)index);

    /*
     * Reset the queue before reprogramming it.
     *
     * virtio 1.0 section 4.2.3.2 requires a driver reconfiguring a live queue
     * to write QUEUE_READY = 0 and wait for it to read back 0.  That is what
     * makes the device drop last_avail_idx.  Without it the device keeps the
     * index while we zero the ring below, so the next time it looks it finds
     * the guest's available index has gone BACKWARDS - QEMU reports "guest
     * moved used index from 1 to 0", marks the device BROKEN, and stops
     * answering it.  The visible symptom is not an error but a freeze: any
     * guest waiting on that device waits for ever, using no CPU.
     *
     * A cold boot programs each queue once and does not need this.  A warm
     * restart does, because the device keeps its state across one while the
     * guest starts over.
     */
    vio_write32(slot, VIRTIO_MMIO_QUEUE_READY, 0);
    for (i = 0; i < 1000; i++) {
        if (vio_read32(slot, VIRTIO_MMIO_QUEUE_READY) == 0) {
            break;
        }
    }

    max = vio_read32(slot, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
    {
        KINFO(("virtio: slot %d queue %d does not exist\n", slot, index));
        return -1;
    }
    if ((ULONG)num > max)
    {
        KINFO(("virtio: slot %d queue %d max is %ld\n", slot, index, max));
        return -1;
    }

    /*
     * Align the descriptor table here rather than trusting the caller.
     *
     * Every ring is declared __attribute__((aligned(16))) and the object
     * files ask for it (2**4 on .bss), but the alignment is a property of
     * the symbol WITHIN its section, and the link places that section
     * wherever it lands -- so the rings come out at whatever alignment the
     * rest of the image leaves over.  Measured: kbd_ring at 0x0005e4a6,
     * which is 2 mod 4, putting the used ring on an odd word.  virtio 1.0
     * requires 16 for the descriptor table, 2 for available and 4 for
     * used; the offsets below preserve that once the base is right.
     *
     * The cost is at most 15 bytes off the end of the caller's buffer, and
     * every ring has room to spare (the tightest is hostfs-link at 228
     * bytes used of 256).
     */
    mem = (UBYTE *)(((ULONG)mem + 15) & ~15UL);

    avail_off = (ULONG)VIRTQ_DESC_SIZE * num;
    used_off = (avail_off + 4 + 2 * (ULONG)num + 15) & ~15UL;

    vq->desc = mem;
    vq->avail = mem + avail_off;
    vq->used = mem + used_off;

    /* A stale ring looks exactly like a valid one to the device. */
    for (i = 0; i < (WORD)(used_off + 4 + VIRTQ_USED_ELEM_SIZE * num); i++)
        mem[i] = 0;

    vio_write32(slot, VIRTIO_MMIO_QUEUE_NUM, (ULONG)num);

    vio_write32(slot, VIRTIO_MMIO_QUEUE_DESC_LOW, (ULONG)vq->desc);
    vio_write32(slot, VIRTIO_MMIO_QUEUE_DESC_HIGH, 0);
    vio_write32(slot, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (ULONG)vq->avail);
    vio_write32(slot, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, 0);
    vio_write32(slot, VIRTIO_MMIO_QUEUE_USED_LOW, (ULONG)vq->used);
    vio_write32(slot, VIRTIO_MMIO_QUEUE_USED_HIGH, 0);

    vio_write32(slot, VIRTIO_MMIO_QUEUE_READY, 1);

    /* The ring is complete; only now may anything else use it. */
    vq->num = num;

    KDEBUG(("virtio: slot %d queue %d num %d desc %p avail %p used %p\n",
            slot, index, num, vq->desc, vq->avail, vq->used));

    return 0;
}

/* Fill one descriptor. Chaining is the caller's business via flags/next. */
void virtq_add(VIRTQ *vq, WORD head, ULONG addr, ULONG len, UWORD flags)
{
    UBYTE *d = vq->desc + (ULONG)head * VIRTQ_DESC_SIZE;

    st64le(d, addr);
    st32le(d + 8, len);
    st16le(d + 12, flags);
    st16le(d + 14, 0);
}

/*
 * Link one descriptor to the next.
 *
 * virtq_add() always writes a "next" of zero, which is correct only for
 * the last descriptor of a chain.  Anything longer than one descriptor
 * has to say where the chain continues, and must also carry
 * VRING_DESC_F_NEXT in the flags virtq_add() was given -- the flag and
 * this field are read independently by the device, and setting only one
 * of them either truncates the chain or walks it into descriptor 0.
 */
void virtq_chain(VIRTQ *vq, WORD head, WORD next)
{
    st16le(vq->desc + (ULONG)head * VIRTQ_DESC_SIZE + 14, (UWORD)next);
}

/*
 * Wait for the next completion on this queue.
 *
 * Returns the head descriptor index and sets *len to the bytes the
 * device wrote, or -1 if nothing completed within the spin bound.
 */
WORD virtq_wait(VIRTQ *vq, ULONG *len)
{
    LONG spin;

    for (spin = 0; spin < VIRTQ_SPIN_LIMIT; spin++)
    {
        WORD head = virtq_poll(vq, len);
        if (head >= 0)
            return head;
    }

    return -1;
}

/*
 * Publish a chain and notify the device.
 *
 * avail.idx is free-running mod 2^16 and is NOT masked by the queue
 * size -- only the ring slot is. QEMU rejects a jump larger than the
 * queue size with "Guest moved used index", which is the signature of
 * having written this field big-endian.
 */
void virtq_kick(VIRTQ *vq, WORD head)
{
    st16le(vq->avail + 4 + 2 * (vq->avail_idx % vq->num), (UWORD)head);

    vq->avail_idx++;
    /* Complete descriptor and ring-entry stores before publishing. */
    __asm__ volatile ("" : : : "memory");
    st16le(vq->avail + 2, vq->avail_idx);
    __asm__ volatile ("" : : : "memory");

    vio_write32(vq->slot, VIRTIO_MMIO_QUEUE_NOTIFY, (ULONG)vq->index);
}

/*
 * Submit a request/response pair and wait for it.
 *
 * This is the shape almost every virtio device uses: one
 * device-readable descriptor carrying the request, chained to one
 * device-writable descriptor for the reply. The order is mandatory --
 * QEMU raises "Incorrect order for descriptors" if a readable
 * descriptor follows a writable one in the same chain.
 *
 * Synchronous by design. Interrupt-driven completion needs the queue
 * bound to an IPL and an ISR ack path; every user so far is a boot-time
 * or low-rate operation where spinning is simpler and cannot deadlock.
 *
 * Returns the byte count the device reported, or -1 on timeout.
 */
LONG virtq_request(VIRTQ *vq, const void *req, ULONG req_len,
                   void *resp, ULONG resp_len)
{
    ULONG len = 0;

    virtq_add(vq, 0, (ULONG)req, req_len, VRING_DESC_F_NEXT);
    virtq_chain(vq, 0, 1);
    virtq_add(vq, 1, (ULONG)resp, resp_len, VRING_DESC_F_WRITE);

    virtq_kick(vq, 0);

    if (virtq_wait(vq, &len) < 0)
        return -1;

    return (LONG)len;
}

/*
 * Check for a completion. Returns the head descriptor index and sets
 * *len to the bytes the device wrote, or -1 if nothing has completed.
 *
 * used.idx is the device's publication barrier: QEMU writes the used
 * element first and only then advances the index.
 */
WORD virtq_poll(VIRTQ *vq, ULONG *len)
{
    UWORD used_idx = ld16le(vq->used + 2);
    UBYTE *elem;
    WORD head;

    if (used_idx == vq->last_used)
        return -1;

    elem = vq->used + 4
         + VIRTQ_USED_ELEM_SIZE * (ULONG)(vq->last_used % vq->num);

    head = (WORD)ld32le(elem);
    if (len)
        *len = ld32le(elem + 4);

    vq->last_used++;
    return head;
}

static const char *virtio_name(ULONG id)
{
    switch (id)
    {
    case VIRTIO_ID_NET:     return "net";
    case VIRTIO_ID_BLK:     return "blk";
    case VIRTIO_ID_CONSOLE: return "console";
    case VIRTIO_ID_RNG:     return "rng";
    case VIRTIO_ID_9P:      return "9p";
    case VIRTIO_ID_GPU:     return "gpu";
    case VIRTIO_ID_INPUT:   return "input";
    }
    return "?";
}

/*
 * Feature negotiation.
 *
 * The handshake QEMU expects is: reset, ACKNOWLEDGE, DRIVER, agree
 * features, FEATURES_OK, then read STATUS back to confirm the device
 * accepted them.  DRIVER_OK comes later, once queues exist.
 *
 * Features are two 32-bit halves selected by *_FEATURES_SEL.  In modern
 * mode the driver's choice is only latched into the device when
 * FEATURES_OK is written, not when DRIVER_FEATURES is -- so the order
 * matters and a driver that writes FEATURES_OK first negotiates
 * nothing.
 *
 * VIRTIO_F_VERSION_1 lives at bit 32, i.e. bit 0 of the high word, and
 * it is what distinguishes a modern driver from a legacy one.  It is
 * always accepted here: this transport is v2 and there is no legacy
 * fallback to drop back to.
 *
 * Returns 0 on success.
 */
static WORD virtio_negotiate(WORD slot, ULONG want_lo)
{
    ULONG dev_lo, dev_hi, drv_lo, drv_hi;
    ULONG status;

    /* Reset, then take ownership one step at a time. */
    vio_write32(slot, VIRTIO_MMIO_STATUS, 0);
    vio_write32(slot, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    vio_write32(slot, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    dev_lo = vio_read32(slot, VIRTIO_MMIO_DEVICE_FEATURES);
    vio_write32(slot, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    dev_hi = vio_read32(slot, VIRTIO_MMIO_DEVICE_FEATURES);

    /*
     * The low word is device-specific, so nothing is taken from it
     * except what the caller asked for on a particular device's behalf
     * -- accepting a feature we do not implement changes the layout the
     * device expects and breaks the queue silently.  WANT_LO is a
     * request, not a demand: a bit the device does not offer is simply
     * not taken, and the driver checks for itself whether it got what
     * it needed (see hostfs_link_attach() and MULTIPORT).
     */
    drv_lo = dev_lo & want_lo;
    drv_hi = dev_hi & (1UL << VIRTIO_F_VERSION_1_BIT);

    /*
     * ACCESS_PLATFORM must be accepted if it is offered, or
     * virtio_validate_features() fails and FEATURES_OK silently never
     * takes effect. It is only offered when the device was created with
     * iommu_platform=on, so normally this bit is absent.
     */
    drv_hi |= dev_hi & (1UL << VIRTIO_F_ACCESS_PLATFORM_BIT);

    if ((drv_hi & (1UL << VIRTIO_F_VERSION_1_BIT)) == 0)
    {
        KINFO(("virtio: slot %d does not offer VERSION_1\n", slot));
        vio_write32(slot, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    vio_write32(slot, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vio_write32(slot, VIRTIO_MMIO_DRIVER_FEATURES, drv_lo);
    vio_write32(slot, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vio_write32(slot, VIRTIO_MMIO_DRIVER_FEATURES, drv_hi);

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK);

    /*
     * The device clears FEATURES_OK if it cannot live with what we
     * chose.  This read-back is the only thing that catches it.
     */
    status = vio_read32(slot, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK))
    {
        KINFO(("virtio: slot %d rejected features (status 0x%02lx)\n",
               slot, status));
        vio_write32(slot, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    KINFO(("virtio: slot %d offered %08lx:%08lx accepted %08lx:%08lx\n",
           slot, dev_hi, dev_lo, drv_hi, drv_lo));

    return 0;
}

/*
 * End-to-end ring self-test against virtio-rng.
 *
 * The simplest device that exists: one queue, hand it a writable
 * buffer, get random bytes back. If the ring layout, the little-endian
 * field encoding, the notify and the used-ring readback are all correct
 * this returns data; if any one of them is wrong it returns nothing or
 * QEMU complains. That makes it a far better check of the virtqueue
 * core than any amount of reading the code.
 */
#define SELFTEST_QSIZE  8
#define SELFTEST_LEN    16

static UBYTE selftest_ring[512] __attribute__((aligned(16)));
static UBYTE selftest_buf[SELFTEST_LEN] __attribute__((aligned(4)));

static void virtio_selftest(WORD slot)
{
    VIRTQ vq;
    ULONG len = 0;
    WORD head, i, spin;

    if (virtq_setup(&vq, slot, 0, SELFTEST_QSIZE, selftest_ring) != 0)
        return;

    /* Queues exist now, so the device may be told the driver is live. */
    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    for (i = 0; i < SELFTEST_LEN; i++)
        selftest_buf[i] = 0;

    /* One device-writable descriptor. */
    virtq_add(&vq, 0, (ULONG)selftest_buf, SELFTEST_LEN, VRING_DESC_F_WRITE);
    virtq_kick(&vq, 0);

    for (spin = 0; spin < 10000; spin++)
    {
        head = virtq_poll(&vq, &len);
        if (head >= 0)
            break;
    }

    if (head < 0)
    {
        KINFO(("virtio: rng self-test TIMED OUT (no completion)\n"));
        return;
    }

    KINFO(("virtio: rng self-test head %d len %ld: "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           head, len,
           selftest_buf[0], selftest_buf[1], selftest_buf[2], selftest_buf[3],
           selftest_buf[4], selftest_buf[5], selftest_buf[6], selftest_buf[7]));
}

/*
 * Probe every transport slot.
 *
 * Empty slots are safe to read: with no backend attached QEMU still
 * returns the real magic, version and vendor ID but a device ID of
 * zero, so enumeration is simply "skip anything reading zero".
 *
 * Returns the number of populated slots found.
 */
WORD virtio_probe(void)
{
    ULONG magic, raw;
    WORD slot, found = 0;

    /*
     * Check the byte order before trusting anything else.  A missing
     * swap is not a subtle failure here -- it is the difference between
     * "virt" and "triv" -- but it becomes a subtle failure the moment
     * we start writing ring addresses, so it is worth failing loudly
     * now rather than debugging a silent queue later.
     */
    raw = VIO_REG(0, VIRTIO_MMIO_MAGIC);
    magic = vio_read32(0, VIRTIO_MMIO_MAGIC);

    if (magic != VIRTIO_MAGIC)
    {
        KINFO(("virtio: bad magic 0x%08lx (raw 0x%08lx)\n", magic, raw));
        if (raw == VIRTIO_MAGIC)
            KINFO(("virtio: register swap is inverted\n"));
        return 0;
    }

    KDEBUG(("virtio: magic ok, raw 0x%08lx swapped 0x%08lx\n", raw, magic));

    for (slot = 0; slot < VIRTIO_MMIO_NB; slot++)
    {
        ULONG id, version, vendor, want_lo;

        if (vio_read32(slot, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC)
            continue;

        id = vio_read32(slot, VIRTIO_MMIO_DEVICE_ID);
        if (id == 0)
            continue;               /* transport present, no backend */

        version = vio_read32(slot, VIRTIO_MMIO_VERSION);
        vendor = vio_read32(slot, VIRTIO_MMIO_VENDOR_ID);

        KINFO(("virtio: slot %d at 0x%08lx: id %ld (%s) v%ld vendor 0x%08lx\n",
               slot, VIRTIO_SLOT(slot), id, virtio_name(id),
               version, vendor));

        if (version != VIRTIO_VERSION_V2)
        {
            KINFO(("virtio: slot %d is not a v2 transport, skipped\n", slot));
            continue;
        }

        /*
         * Device-specific features, asked for before the driver that
         * needs them runs: FEATURES_OK is written during negotiation
         * and a feature not taken there cannot be taken afterwards.
         * MULTIPORT is what gives a virtio-console its control queues
         * and its ports past 0, and QEMU reserves port 0 for a
         * `virtconsole`, so a `virtserialport` is unreachable without
         * it -- see the handshake note in hostfs_link.c.
         */
        if (id == VIRTIO_ID_CONSOLE)
            want_lo = 1UL << VIRTIO_CONSOLE_F_MULTIPORT_BIT;
        else
            want_lo = 0;

        if (virtio_negotiate(slot, want_lo) == 0)
        {
            found++;
            if (id == VIRTIO_ID_RNG)
                virtio_selftest(slot);
            if (id == VIRTIO_ID_INPUT)
                virtio_input_attach(slot);
            if (id == VIRTIO_ID_9P)
                virtio_9p_attach(slot);
            if (id == VIRTIO_ID_CONSOLE)
                hostfs_link_attach(slot);
#if CONF_WITH_VIRTIO_BLK
            if (id == VIRTIO_ID_BLK)
                virtio_blk_attach(slot);
#endif
            if (id == VIRTIO_ID_GPU)
            {
                if (virtio_gpu_init(slot, QEMUVIRT_ARGB_FB,
                                    &qemuvirt_screen_w,
                                    &qemuvirt_screen_h) == 0)
                    virtio_gpu_build_ctab();
            }
        }
    }

    KINFO(("virtio: %d device(s)\n", found));
    return found;
}

#endif /* MACHINE_QEMU_VIRT */
