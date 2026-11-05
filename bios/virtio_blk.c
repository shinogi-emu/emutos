/*
 * virtio_blk.c - virtio-blk block device driver
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * A virtio-blk device is a flat array of 512-byte sectors addressed by
 * LBA, which is exactly what bios/disk.c already expects from a bus
 * driver.  Everything above this file -- the MBR and Atari partition
 * scans, the BPB, FAT12/16 -- is stock EmuTOS.
 *
 * VIRTIO_BLK_T_IN and VIRTIO_BLK_T_OUT share one three-descriptor
 * chain and differ only in which way the payload descriptor points:
 * device-writable for a read, device-readable for a write.  Getting
 * that flag wrong does not fail loudly -- the device simply transfers
 * in the direction the descriptor claims -- so it is derived from the
 * request type in one place rather than passed in.
 *
 * Writes require a launcher-tagged raw image (shinogi.disk1..4 or
 * shinogi.floppya/b) or the developer-only CONF_WITH_VIRTIO_BLK_WRITE
 * override. The negotiated backend read-only feature always wins. Ordinary
 * untagged/vvfat devices remain protected; see include/config.h.
 *
 * Every multi-byte field of a request header is little-endian on the
 * wire and this is a big-endian target, so the header is assembled byte
 * by byte.  Do not be tempted to overlay a struct: the 64-bit sector
 * field in particular is not two ULONGs in the order a cast would give.
 */

#include "emutos.h"
#include "virtio.h"
#include "virtio_blk.h"
#include "disk.h"
#include "gemerror.h"
#include "biosdefs.h"
#include "string.h"
#include "kprint.h"

#if CONF_WITH_VIRTIO_BLK

#define VBLK_QSIZE  8

/*
 * Descriptor indices within the single chain this driver builds.  The
 * order is mandatory: QEMU rejects a chain whose device-readable
 * descriptors do not all precede its device-writable ones.
 */
#define VBLK_D_HDR      0       /* 16-byte header, device-readable */
#define VBLK_D_DATA     1       /* payload,        direction by type */
#define VBLK_D_STATUS   2       /* 1 status byte,  device-writable */

#define VBLK_HDR_SIZE   16

typedef struct
{
    WORD readonly, image_write, floppy;
    char id[21];
    WORD  slot;                 /* transport slot, -1 if unused */
    ULONG capacity;             /* size in 512-byte sectors     */
    VIRTQ vq;
    UBYTE ring[512] __attribute__((aligned(16)));
    UBYTE hdr[VBLK_HDR_SIZE] __attribute__((aligned(4)));
    UBYTE status;
} VBLK;

static VBLK vblk_storage[VIRTIO_BLK_MAXDEV];
static VBLK *vblk[VIRTIO_BLK_MAXDEV];
static WORD vblk_count;

/* Little-endian store, byte by byte.  See the file header. */
static void vblk_st32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)((v >> 8) & 0xff);
    p[2] = (UBYTE)((v >> 16) & 0xff);
    p[3] = (UBYTE)((v >> 24) & 0xff);
}

/*
 * Return the device for a relative device number, or NULL.
 *
 * Devices are numbered in the order virtio_probe() found them, not by
 * transport slot: the slot a device lands in depends on the order of
 * QEMU's command line and is not something a drive letter should follow.
 */
static VBLK *vblk_dev(UWORD dev)
{
    if (dev >= VIRTIO_BLK_MAXDEV)
        return NULL;
    return vblk[dev];
}

/*
 * Issue one request and wait for it.
 *
 * The chain is three descriptors, which is one more than
 * virtq_request() builds, so it is assembled here: header out, payload
 * in, status in.  Splitting the status off from the payload is not
 * optional -- it is a separate descriptor by the device specification,
 * and a device that finds only two writes its status over the last byte
 * of the data.
 *
 * Returns 0 if the device reported success.
 */
static LONG vblk_request(VBLK *v, ULONG type, ULONG sector, UBYTE *buf,
                         ULONG bytes)
{
    ULONG len = 0;
    ULONG expect;
    UWORD dflags;
    LONG fault;
    WORD head;

    /*
     * A read has the device fill the payload, so that descriptor is
     * device-writable and the device reports the payload plus the
     * status byte as written.  A write hands the payload to the device,
     * so the descriptor is device-readable and the only byte written
     * back is the status.
     */
    if (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_GET_ID)
    {
        dflags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
        expect = bytes + 1;
    }
    else
    {
        dflags = VRING_DESC_F_NEXT;
        expect = 1;
    }

    /* type, then a reserved word, then the 64-bit sector number. */
    vblk_st32(v->hdr, type);
    vblk_st32(v->hdr + 4, 0UL);
    vblk_st32(v->hdr + 8, sector);
    vblk_st32(v->hdr + 12, 0UL);        /* we never address above 2TB */

    /*
     * Preset to a value the device never writes, so a status left
     * untouched cannot be mistaken for VIRTIO_BLK_S_OK.
     */
    v->status = 0xff;

    virtq_add(&v->vq, VBLK_D_HDR, (ULONG)v->hdr, VBLK_HDR_SIZE,
              VRING_DESC_F_NEXT);
    virtq_chain(&v->vq, VBLK_D_HDR, VBLK_D_DATA);

    virtq_add(&v->vq, VBLK_D_DATA, (ULONG)buf, bytes, dflags);
    virtq_chain(&v->vq, VBLK_D_DATA, VBLK_D_STATUS);

    virtq_add(&v->vq, VBLK_D_STATUS, (ULONG)&v->status, 1UL,
              VRING_DESC_F_WRITE);

    virtq_kick(&v->vq, VBLK_D_HDR);

    /* Failures are reported as the fault of the direction we asked for. */
    fault = (type == VIRTIO_BLK_T_IN) ? EREADF : EWRITF;

    head = virtq_wait(&v->vq, &len);
    if (head < 0)
    {
        KINFO(("virtio-blk: slot %d request timed out\n", v->slot));
        return fault;
    }

    if (v->status != VIRTIO_BLK_S_OK)
    {
        KINFO(("virtio-blk: slot %d sector %lu status %d\n",
               v->slot, sector, v->status));
        return fault;
    }

    /*
     * The device reports the bytes it wrote into our buffers: for a read
     * that is the payload plus the status byte, for a write the status
     * byte alone.  Anything short means a partial transfer -- on a read
     * the tail of the buffer still holds whatever was there before, and
     * on a write the medium does.
     */
    if (len < expect)
    {
        KINFO(("virtio-blk: slot %d short transfer %lu of %lu\n",
               v->slot, len, expect));
        return fault;
    }

    return 0;
}

/*
 * Read or write sectors.
 *
 * count is a sector count and sector sizes are 512 bytes, so the byte
 * count exceeds a 16-bit int for anything past 64 sectors.  It is
 * computed in ULONG throughout; with -mshort an int expression here
 * would wrap silently.
 */
LONG virtio_blk_rw(WORD rw, LONG sector, WORD count, UBYTE *buf, WORD dev)
{
    VBLK *v = vblk_dev((UWORD)dev);
    ULONG first = (ULONG)sector;
    ULONG type = (rw & RW_RW) ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    ULONG bytes;

    if (!v)
        return EUNDEV;

#if !CONF_WITH_VIRTIO_BLK_WRITE
    if (type == VIRTIO_BLK_T_OUT && !v->image_write)
        return EWRPRO;
#endif
    if (type == VIRTIO_BLK_T_OUT && v->readonly) return EWRPRO;

    if (count <= 0)
        return 0;

    bytes = (ULONG)(UWORD)count * VIRTIO_BLK_SECSIZE;

    /*
     * The device would fail the request itself, but it fails it as an
     * I/O error indistinguishable from a real one; a request off the
     * end of the medium is worth naming.
     */
    if (first > v->capacity || (ULONG)(UWORD)count > v->capacity - first)
    {
        KDEBUG(("virtio_blk_rw(): sector %lu+%d beyond capacity %lu\n",
                first, count, v->capacity));
        return ESECNF;
    }

    return vblk_request(v, type, first, buf, bytes);
}

LONG virtio_blk_ioctl(UWORD dev, UWORD ctrl, void *arg)
{
    VBLK *v = vblk_dev(dev);
    ULONG *info = arg;

    if (!v || v->floppy >= 0)
        return EUNDEV;

    switch(ctrl)
    {
    case GET_DISKINFO:
        info[0] = v->capacity;
        info[1] = VIRTIO_BLK_SECSIZE;
        return 0;
    case GET_DISKNAME:
        strcpy(arg, "virtio-blk");
        return 0;
    case GET_MEDIACHANGE:
        /*
         * The medium is whatever QEMU was pointed at when it started
         * and cannot be swapped from the guest side, so it never
         * changes.  Reporting anything else would make the BDOS discard
         * its buffers on every access.
         */
        return MEDIANOCHANGE;
    case CHECK_DEVICE:
        return 0;
    }

    return ERR;
}

/*
 * Adopt one device found by virtio_probe().
 *
 * The transport has already been reset and its features negotiated; all
 * that is left is the queue and DRIVER_OK.  Capacity is read from the
 * device configuration space, which is part of the register window and
 * therefore goes through vio_read32() like any other register -- the
 * ring rules do not apply to it.
 */
void virtio_blk_attach(WORD slot)
{
    VBLK *v;
    ULONG cap_lo, cap_hi;
    WORD index, i;

    if (vblk_count >= VIRTIO_BLK_MAXDEV)
    {
        KINFO(("virtio-blk: slot %d ignored, %d devices already\n",
               slot, vblk_count));
        return;
    }

    v = &vblk_storage[vblk_count];
    v->slot = -1;

    if (virtq_setup(&v->vq, slot, 0, VBLK_QSIZE, v->ring) != 0)
    {
        KINFO(("virtio-blk: slot %d queue setup failed\n", slot));
        return;
    }

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    cap_lo = vio_read32(slot, VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CFG_CAPACITY_LO);
    cap_hi = vio_read32(slot, VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CFG_CAPACITY_HI);

    /*
     * A sector number is a ULONG everywhere above this driver, so a
     * medium larger than 2TB is clamped rather than wrapped.  Nothing
     * this project drives is anywhere near that; the clamp exists so
     * that if it ever is, the disk is short rather than nonsense.
     */
    if (cap_hi != 0)
    {
        KINFO(("virtio-blk: slot %d capacity %lu:%08lx clamped\n",
               slot, cap_hi, cap_lo));
        cap_lo = 0xffffffffUL;
    }

    v->capacity = cap_lo;
    v->slot = slot;
    v->floppy = -1;
    vio_write32(slot, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    v->readonly = (vio_read32(slot, VIRTIO_MMIO_DEVICE_FEATURES) & (1UL << 5)) != 0;
    memset(v->id, 0, sizeof(v->id));
    if (vblk_request(v, VIRTIO_BLK_T_GET_ID, 0, (UBYTE *)v->id, 20UL) < 0) {
        KINFO(("virtio-blk: slot %d could not read image identity\n", slot));
        vblk_count++;
        return;
    }
    index = -1;
    for (i = 0; i < 4; i++)
    {
        char name[] = "shinogi.disk1";
        name[sizeof(name) - 2] = '1' + i;
        if (!strcmp(v->id, name)) { index = i; v->image_write = 1; break; }
    }
    if (!strcmp(v->id, "shinogi.floppya") || !strcmp(v->id, "shinogi.floppyb"))
    {
        v->floppy = v->id[sizeof("shinogi.floppya") - 2] == 'a' ? 0 : 1;
        index = 4 + v->floppy;
        v->image_write = 1;
        if (cap_lo != 1440UL && cap_lo != 2880UL) index = -2;
    }
    if (index == -1)
        for (i = 0; i < 4; i++)
            if (!vblk[i]) { index = i; break; }
    if (index < 0 || vblk[index])
    {
        KINFO(("virtio-blk: invalid or duplicate image identity %s\n", v->id));
        vblk_count++; /* rings remain owned by the ignored device */
        return;
    }
    vblk[index] = v;

    KINFO(("virtio-blk: device %d at slot %d, %lu sectors\n",
           vblk_count, slot, v->capacity));

    vblk_count++;
}

/* Image attachment is fixed for one emulator run. A restart rebuilds all
 * BIOS/BPB/cache state; live QMP media replacement is deliberately unsupported. */
ULONG virtio_blk_floppy_capacity(WORD drive)
{
    VBLK *v;
    if (drive < 0 || drive > 1) return 0;
    v = vblk_dev(4 + drive);
    return v ? v->capacity : 0;
}

LONG virtio_blk_floppy_rw(WORD rw, LONG sector, WORD count, UBYTE *buf, WORD drive)
{
    if (drive < 0 || drive > 1 || !virtio_blk_floppy_capacity(drive)) return EUNDEV;
    return virtio_blk_rw(rw, sector, count, buf, 4 + drive);
}

#endif /* CONF_WITH_VIRTIO_BLK */
