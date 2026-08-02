/*
 * virtio.h - virtio-mmio transport
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#ifdef MACHINE_QEMU_VIRT

/*
 * ENDIANNESS
 *
 * This is modern (v2) virtio: the machine constructs every transport
 * with force-legacy=false.  Two separate byte-order problems follow,
 * and they are easy to conflate:
 *
 *  1. The REGISTER WINDOW is DEVICE_LITTLE_ENDIAN.  QEMU's memory core
 *     byte-swaps every 32-bit access whose endianness differs from the
 *     region's, so on m68k each register read and write is swapped in
 *     both directions.  vio_read32()/vio_write32() undo that.
 *
 *  2. The RING STRUCTURES in guest RAM are little-endian by spec,
 *     independently of the transport.  Those are DMA, not MMIO, so the
 *     memory core never touches them and the driver must swap them
 *     itself.
 *
 * Neither is optional and neither substitutes for the other.  Getting
 * either wrong produces plausible-looking garbage rather than a clean
 * failure, so nothing accesses a register or a ring field except
 * through the helpers below.
 *
 * Sanity check: the guest must read VIRTIO_MMIO_MAGIC as
 * VIRTIO_MAGIC (0x74726976) once vio_read32() has swapped it.  Reading
 * the raw window without swapping yields 0x76697274 -- "virt" in
 * human-readable order, which is the giveaway that a swap is missing.
 */

/* Transport layout -- see hw/m68k/virt.c */
#define VIRTIO_MMIO_BASE    0xff010000UL
#define VIRTIO_MMIO_STRIDE  0x200UL
#define VIRTIO_MMIO_NB      128

#define VIRTIO_SLOT(n)  (VIRTIO_MMIO_BASE + ((ULONG)(n) * VIRTIO_MMIO_STRIDE))

/* Registers -- include/standard-headers/linux/virtio_mmio.h */
#define VIRTIO_MMIO_MAGIC               0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc
#define VIRTIO_MMIO_CONFIG              0x100

/* Constants -- include/hw/virtio/virtio-mmio.h */
#define VIRTIO_MAGIC        0x74726976UL    /* after the swap  */
#define VIRTIO_MAGIC_RAW    0x76697274UL    /* before it       */
#define VIRTIO_VENDOR       0x554d4551UL    /* "QEMU"          */
#define VIRTIO_VERSION_V2   2

/* Status bits -- include/standard-headers/linux/virtio_config.h */
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_NEEDS_RESET   0x40
#define VIRTIO_STATUS_FAILED        0x80

/* Interrupt status bits */
#define VIRTIO_INT_VRING    0x01
#define VIRTIO_INT_CONFIG   0x02

/* Device IDs we care about */
#define VIRTIO_ID_NET       1
#define VIRTIO_ID_BLK       2
#define VIRTIO_ID_CONSOLE   3
#define VIRTIO_ID_RNG       4
#define VIRTIO_ID_9P        9
#define VIRTIO_ID_GPU       16
#define VIRTIO_ID_INPUT     18

/*
 * Feature bits.  VIRTIO_F_VERSION_1 is bit 32, i.e. bit 0 of the high
 * feature word, which is why features are negotiated as two 32-bit
 * halves selected by *_FEATURES_SEL.
 */
#define VIRTIO_F_VERSION_1_BIT       0  /* bit 32, within the high word */
#define VIRTIO_F_ACCESS_PLATFORM_BIT 1  /* bit 33 */

/*
 * Split virtqueue.
 *
 * Layout QEMU expects (hw/virtio/virtio.c:65-95). Every field is
 * little-endian for a VERSION_1 device regardless of target, which is a
 * separate mechanism from the register swap and must be applied
 * separately.
 *
 *   desc[i]   16 bytes: addr(8) len(4) flags(2) next(2)
 *   avail      flags(2) idx(2) ring[num](2 each)   [+ used_event(2)]
 *   used       flags(2) idx(2) ring[num](8 each)   [+ avail_event(2)]
 *
 * The trailing event fields exist only if VIRTIO_RING_F_EVENT_IDX was
 * negotiated. We do not negotiate it, so they are absent and QEMU never
 * reads them.
 */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VRING_AVAIL_F_NO_INTERRUPT  1
#define VRING_USED_F_NO_NOTIFY      1

#define VIRTQ_DESC_SIZE     16
#define VIRTQ_USED_ELEM_SIZE 8

typedef struct
{
    WORD slot;              /* transport slot        */
    WORD index;             /* queue index           */
    WORD num;               /* queue size            */
    UWORD avail_idx;        /* our copy of avail.idx */
    UWORD last_used;        /* our copy of used.idx  */
    UBYTE *desc;
    UBYTE *avail;
    UBYTE *used;
} VIRTQ;

ULONG vio_read32(WORD slot, WORD reg);
void vio_write32(WORD slot, WORD reg, ULONG value);

WORD virtio_probe(void);
WORD virtq_setup(VIRTQ *vq, WORD slot, WORD index, WORD num, UBYTE *mem);
void virtq_add(VIRTQ *vq, WORD head, ULONG addr, ULONG len, UWORD flags);
void virtq_kick(VIRTQ *vq, WORD head);
WORD virtq_poll(VIRTQ *vq, ULONG *len);
LONG virtq_request(VIRTQ *vq, const void *req, ULONG req_len,
                   void *resp, ULONG resp_len);

/* Spin bound for synchronous requests. */
#define VIRTQ_SPIN_LIMIT    20000000L

#endif /* MACHINE_QEMU_VIRT */

#endif /* VIRTIO_H */
