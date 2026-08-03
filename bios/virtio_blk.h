/*
 * virtio_blk.h - virtio-blk block device driver
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "portab.h"

#if CONF_WITH_VIRTIO_BLK

/*
 * Request types -- include/standard-headers/linux/virtio_blk.h
 */
#define VIRTIO_BLK_T_IN     0UL     /* read from the device  */
#define VIRTIO_BLK_T_OUT    1UL     /* write to the device   */

/* Status byte the device writes as the last descriptor of a chain */
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/*
 * Device configuration space, relative to VIRTIO_MMIO_CONFIG.
 * capacity is in 512-byte sectors regardless of the block size.
 */
#define VIRTIO_BLK_CFG_CAPACITY_LO  0x00
#define VIRTIO_BLK_CFG_CAPACITY_HI  0x04

/*
 * The number of virtio-blk devices this driver will adopt.  The bus has
 * room for DEVICES_PER_BUS of them; the limit here is only about the
 * per-device ring and header buffers, which are static.
 */
#define VIRTIO_BLK_MAXDEV   4

/* Sector size.  We do not negotiate VIRTIO_BLK_F_BLK_SIZE, so it is 512. */
#define VIRTIO_BLK_SECSIZE  512UL

void virtio_blk_attach(WORD slot);
LONG virtio_blk_ioctl(UWORD dev, UWORD ctrl, void *arg);
LONG virtio_blk_rw(WORD rw, LONG sector, WORD count, UBYTE *buf, WORD dev);

#endif /* CONF_WITH_VIRTIO_BLK */

#endif /* VIRTIO_BLK_H */
