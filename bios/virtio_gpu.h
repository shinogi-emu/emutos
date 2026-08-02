/*
 * virtio_gpu.h - virtio-gpu 2D
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#ifdef MACHINE_QEMU_VIRT

/*
 * Command codes (include/standard-headers/linux/virtio_gpu.h)
 *
 * Every u32 and u64 in a command or response is LITTLE-ENDIAN, exactly
 * like the ring -- not guest-native. The u8 fields need no swap.
 */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106

#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

/* Success is 0x1100..0x11ff; anything from 0x1200 is an error. */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_ERR_BASE                0x1200

/*
 * Pixel formats.
 *
 * The name describes the BYTE SEQUENCE in the guest buffer, low address
 * first, and is independent of guest endianness -- QEMU's PIXMAN_BE_*
 * macros are defined twice under opposite HOST_BIG_ENDIAN branches so
 * that the bytes always land in the named order. Pixel data itself is
 * never swapped; TRANSFER_TO_HOST_2D is a raw copy.
 *
 * X8R8G8B8 therefore matches a natural big-endian longword store of
 * 0x00RRGGBB, which is what an m68k writes with a plain move.l. That is
 * why it is the format used here.
 *
 * Do not trust this reasoning alone -- the test pattern below verifies
 * it against the running device.
 */
#define VIRTIO_GPU_FORMAT_B8G8R8A8  1
#define VIRTIO_GPU_FORMAT_B8G8R8X8  2
#define VIRTIO_GPU_FORMAT_A8R8G8B8  3
#define VIRTIO_GPU_FORMAT_X8R8G8B8  4   /* bytes X,R,G,B -- our choice */

/* Command header is 24 bytes; payload offsets are from the request start. */
#define GPU_HDR_SIZE        24
#define GPU_HDR_TYPE        0
#define GPU_HDR_FLAGS       4
#define GPU_HDR_FENCE_ID    8
#define GPU_HDR_CTX_ID      16

/* RESOURCE_CREATE_2D -- 40 bytes */
#define GPU_CREATE_ID       24
#define GPU_CREATE_FORMAT   28
#define GPU_CREATE_WIDTH    32
#define GPU_CREATE_HEIGHT   36
#define GPU_CREATE_SIZE     40

/* RESOURCE_ATTACH_BACKING -- 32 bytes + nr_entries * 16 */
#define GPU_ATTACH_ID       24
#define GPU_ATTACH_NR       28
#define GPU_ATTACH_ENT_ADDR 32
#define GPU_ATTACH_ENT_LEN  40
#define GPU_ATTACH_SIZE     48  /* one entry */

/* SET_SCANOUT -- 48 bytes */
#define GPU_SCANOUT_X       24
#define GPU_SCANOUT_Y       28
#define GPU_SCANOUT_W       32
#define GPU_SCANOUT_H       36
#define GPU_SCANOUT_ID      40
#define GPU_SCANOUT_RES     44
#define GPU_SCANOUT_SIZE    48

/* TRANSFER_TO_HOST_2D -- 56 bytes */
#define GPU_XFER_X          24
#define GPU_XFER_Y          28
#define GPU_XFER_W          32
#define GPU_XFER_H          36
#define GPU_XFER_OFFSET     40  /* u64 */
#define GPU_XFER_RES        48
#define GPU_XFER_SIZE       56

/* RESOURCE_FLUSH -- 48 bytes */
#define GPU_FLUSH_X         24
#define GPU_FLUSH_Y         28
#define GPU_FLUSH_W         32
#define GPU_FLUSH_H         36
#define GPU_FLUSH_RES       40
#define GPU_FLUSH_SIZE      48

/* Scanout rects must be at least 16x16. */
#define GPU_MIN_DIM         16

#define GPU_RESOURCE_ID     1
#define GPU_SCANOUT_INDEX   0

WORD virtio_gpu_init(WORD slot, ULONG fb, WORD width, WORD height);
void virtio_gpu_flush(WORD x, WORD y, WORD w, WORD h);
void virtio_gpu_testpattern(ULONG fb, WORD w, WORD h);

void virtio_gpu_build_ctab(void);
void virtio_gpu_present(void);

#endif /* MACHINE_QEMU_VIRT */

#endif /* VIRTIO_GPU_H */
