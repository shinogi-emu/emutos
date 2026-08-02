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
static WORD virtio_negotiate(WORD slot)
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
     * Take nothing from the low word yet.  Every low-word bit is
     * device-specific and each driver will opt in to what it needs;
     * accepting a feature we do not implement changes the layout the
     * device expects and breaks the queue silently.
     */
    drv_lo = 0;
    drv_hi = dev_hi & (1UL << VIRTIO_F_VERSION_1_BIT);

    if (drv_hi == 0)
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
        ULONG id, version, vendor;

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

        if (virtio_negotiate(slot) == 0)
            found++;
    }

    KINFO(("virtio: %d device(s)\n", found));
    return found;
}

#endif /* MACHINE_QEMU_VIRT */
