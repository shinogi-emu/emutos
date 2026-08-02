/*
 * virtio_gpu.c - virtio-gpu 2D
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#define ENABLE_KDEBUG

#include "emutos.h"
#include "virtio.h"
#include "virtio_gpu.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

/*
 * Command buffers.
 *
 * Static rather than allocated because the display is brought up from
 * machine_init(), before the memory pool exists.
 *
 * The request buffer must always be sent at the exact size of the
 * command struct. Two distinct traps otherwise, neither obvious:
 * a device-readable region shorter than the 24-byte header stalls the
 * control queue permanently -- the command is never marked finished and
 * sits at the head of the queue forever -- and one that is at least 24
 * bytes but shorter than the command struct is answered OK_NODATA while
 * doing nothing at all.
 */
static UBYTE gpu_req[64] __attribute__((aligned(4)));
static UBYTE gpu_resp[64] __attribute__((aligned(4)));
static UBYTE gpu_ring[1024] __attribute__((aligned(16)));

static VIRTQ gpu_vq;
static WORD gpu_ready;
static WORD gpu_w, gpu_h;

/* Command fields are little-endian, like the ring. */
static void gpu_set32(WORD off, ULONG v)
{
    gpu_req[off] = (UBYTE)(v & 0xff);
    gpu_req[off + 1] = (UBYTE)((v >> 8) & 0xff);
    gpu_req[off + 2] = (UBYTE)((v >> 16) & 0xff);
    gpu_req[off + 3] = (UBYTE)((v >> 24) & 0xff);
}

static ULONG gpu_get32(const UBYTE *p, WORD off)
{
    return (ULONG)p[off] | ((ULONG)p[off + 1] << 8)
         | ((ULONG)p[off + 2] << 16) | ((ULONG)p[off + 3] << 24);
}

/* Start a command: zero the buffer and lay down the header. */
static void gpu_begin(ULONG type, WORD size)
{
    WORD i;

    for (i = 0; i < size; i++)
        gpu_req[i] = 0;

    gpu_set32(GPU_HDR_TYPE, type);
}

/*
 * Send a command and check the reply.
 *
 * Success is any response type in 0x1100..0x11ff. Returns 0 on success.
 */
static WORD gpu_send(WORD size)
{
    ULONG resp;
    LONG got;
    WORD i;

    for (i = 0; i < GPU_HDR_SIZE; i++)
        gpu_resp[i] = 0;

    got = virtq_request(&gpu_vq, gpu_req, (ULONG)size,
                        gpu_resp, GPU_HDR_SIZE);
    if (got < 0)
    {
        KINFO(("gpu: command 0x%04lx timed out\n", gpu_get32(gpu_req, 0)));
        return -1;
    }

    resp = gpu_get32(gpu_resp, GPU_HDR_TYPE);
    if (resp >= VIRTIO_GPU_RESP_ERR_BASE)
    {
        KINFO(("gpu: command 0x%04lx failed, response 0x%04lx\n",
               gpu_get32(gpu_req, 0), resp));
        return -1;
    }

    return 0;
}

/*
 * Bring the display up.
 *
 * Order matters beyond the obvious: RESOURCE_FLUSH is a silent no-op
 * until SET_SCANOUT has run, because flush only touches scanouts whose
 * bit is set in the resource's scanout mask, and only a successful
 * SET_SCANOUT sets it. Equally, nothing the guest writes is visible
 * until TRANSFER_TO_HOST_2D copies it -- the scanout surface points at
 * QEMU's own image, not at our buffer.
 */
WORD virtio_gpu_init(WORD slot, ULONG fb, WORD width, WORD height)
{
    if (width < GPU_MIN_DIM || height < GPU_MIN_DIM)
    {
        KINFO(("gpu: %dx%d is below the 16x16 minimum\n", width, height));
        return -1;
    }

    /* Control queue is index 0. The cursor queue (1) is left alone. */
    if (virtq_setup(&gpu_vq, slot, 0, 16, gpu_ring) != 0)
        return -1;

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    gpu_begin(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, GPU_CREATE_SIZE);
    gpu_set32(GPU_CREATE_ID, GPU_RESOURCE_ID);
    gpu_set32(GPU_CREATE_FORMAT, VIRTIO_GPU_FORMAT_X8R8G8B8);
    gpu_set32(GPU_CREATE_WIDTH, (ULONG)width);
    gpu_set32(GPU_CREATE_HEIGHT, (ULONG)height);
    if (gpu_send(GPU_CREATE_SIZE) != 0)
        return -1;

    gpu_begin(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, GPU_ATTACH_SIZE);
    gpu_set32(GPU_ATTACH_ID, GPU_RESOURCE_ID);
    gpu_set32(GPU_ATTACH_NR, 1);
    gpu_set32(GPU_ATTACH_ENT_ADDR, fb);         /* low half of the u64 */
    gpu_set32(GPU_ATTACH_ENT_ADDR + 4, 0);
    gpu_set32(GPU_ATTACH_ENT_LEN, (ULONG)width * height * 4);
    if (gpu_send(GPU_ATTACH_SIZE) != 0)
        return -1;

    gpu_begin(VIRTIO_GPU_CMD_SET_SCANOUT, GPU_SCANOUT_SIZE);
    gpu_set32(GPU_SCANOUT_W, (ULONG)width);
    gpu_set32(GPU_SCANOUT_H, (ULONG)height);
    gpu_set32(GPU_SCANOUT_ID, GPU_SCANOUT_INDEX);
    gpu_set32(GPU_SCANOUT_RES, GPU_RESOURCE_ID);
    if (gpu_send(GPU_SCANOUT_SIZE) != 0)
        return -1;

    gpu_w = width;
    gpu_h = height;
    gpu_ready = 1;

    KINFO(("gpu: %dx%d scanout up, backing 0x%08lx\n", width, height, fb));
    return 0;
}

/*
 * Push one rectangle to the host.
 *
 * The transfer offset is into the guest backing buffer, and for a
 * partial-width rect QEMU advances the source by the full resource
 * stride per row rather than by the rect width -- so the offset must be
 * the address of the rect's first pixel, not zero.
 */
void virtio_gpu_flush(WORD x, WORD y, WORD w, WORD h)
{
    ULONG offset;

    if (!gpu_ready)
        return;

    offset = ((ULONG)y * gpu_w + x) * 4;

    gpu_begin(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, GPU_XFER_SIZE);
    gpu_set32(GPU_XFER_X, (ULONG)x);
    gpu_set32(GPU_XFER_Y, (ULONG)y);
    gpu_set32(GPU_XFER_W, (ULONG)w);
    gpu_set32(GPU_XFER_H, (ULONG)h);
    gpu_set32(GPU_XFER_OFFSET, offset);
    gpu_set32(GPU_XFER_OFFSET + 4, 0);
    gpu_set32(GPU_XFER_RES, GPU_RESOURCE_ID);
    if (gpu_send(GPU_XFER_SIZE) != 0)
        return;

    gpu_begin(VIRTIO_GPU_CMD_RESOURCE_FLUSH, GPU_FLUSH_SIZE);
    gpu_set32(GPU_FLUSH_X, (ULONG)x);
    gpu_set32(GPU_FLUSH_Y, (ULONG)y);
    gpu_set32(GPU_FLUSH_W, (ULONG)w);
    gpu_set32(GPU_FLUSH_H, (ULONG)h);
    gpu_set32(GPU_FLUSH_RES, GPU_RESOURCE_ID);
    gpu_send(GPU_FLUSH_SIZE);
}

/*
 * Write a known pattern and show it.
 *
 * The pixel format question cannot be settled by reading QEMU's format
 * names: the PIXMAN_BE_* macros are defined twice under opposite
 * HOST_BIG_ENDIAN branches, so the same identifier is a different
 * constant depending on the machine QEMU was built on. The only answer
 * that survives a host change is to write known bytes and look at what
 * lands, which is what this does -- four flat quadrants whose values are
 * unmistakable if a channel is transposed.
 *
 * Written as big-endian longwords 0x00RRGGBB, which is what a plain
 * move.l produces on m68k. If X8R8G8B8 is the right choice, the colours
 * come out as named.
 */
void virtio_gpu_testpattern(ULONG fb, WORD w, WORD h)
{
    volatile ULONG *p = (volatile ULONG *)fb;
    WORD x, y;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            ULONG c;

            if (y < h / 2)
                c = (x < w / 2) ? 0x00ff0000UL   /* red    */
                                : 0x0000ff00UL;  /* green  */
            else
                c = (x < w / 2) ? 0x000000ffUL   /* blue   */
                                : 0x00ffffffUL;  /* white  */

            p[(ULONG)y * w + x] = c;
        }
    }

    virtio_gpu_flush(0, 0, w, h);
}

#endif /* MACHINE_QEMU_VIRT */
