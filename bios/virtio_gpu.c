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
#include "qemuvirt.h"
#include "tosvars.h"

#ifdef MACHINE_QEMU_VIRT

#include "gpu_damage.h"

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
/* Sized for the largest reply, which is GET_DISPLAY_INFO's 408 bytes. */
static UBYTE gpu_resp[GPU_DISPINFO_SIZE] __attribute__((aligned(4)));
static UBYTE gpu_ring[1024] __attribute__((aligned(16)));

static VIRTQ gpu_vq;
static WORD gpu_ready;
static WORD gpu_w, gpu_h;
static WORD gpu_full_update;
static WORD gpu_present_busy;
static WORD gpu_active_frames;
static WORD gpu_idle_frames;

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
static WORD gpu_send_resp(WORD size, WORD resplen)
{
    ULONG resp;
    LONG got;
    WORD i;

    for (i = 0; i < resplen; i++)
        gpu_resp[i] = 0;

    got = virtq_request(&gpu_vq, gpu_req, (ULONG)size,
                        gpu_resp, (ULONG)resplen);
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

static WORD gpu_send(WORD size)
{
    return gpu_send_resp(size, GPU_HDR_SIZE);
}

/*
 * Ask the host what size it wants.
 *
 * GET_DISPLAY_INFO returns a rect per scanout; QEMU fills the first from
 * the virtio-gpu-device xres/yres properties, which is how the launcher
 * chooses the mode. Anything unusable -- no reply, a disabled scanout,
 * a size below the 16x16 minimum or past what the ARGB surface is
 * budgeted for -- leaves the caller's defaults alone rather than
 * producing a screen that cannot be displayed.
 *
 * The width is rounded DOWN to a multiple of 8: update_rez_dependent()
 * computes the stride as V_REZ_HZ / 8 * v_planes, so an odd width would
 * shear every line.
 */
WORD virtio_gpu_preferred(WORD slot, WORD *width, WORD *height)
{
    ULONG w, h;

    gpu_begin(VIRTIO_GPU_CMD_GET_DISPLAY_INFO, GPU_HDR_SIZE);
    if (gpu_send_resp(GPU_HDR_SIZE, GPU_DISPINFO_SIZE) != 0)
        return -1;

    /* pmodes[0] follows the header: rect is x, y, width, height. */
    if (gpu_get32(gpu_resp, GPU_DISPINFO_PM0 + GPU_DISPINFO_ENABLED) == 0)
        return -1;

    w = gpu_get32(gpu_resp, GPU_DISPINFO_PM0 + GPU_DISPINFO_W);
    h = gpu_get32(gpu_resp, GPU_DISPINFO_PM0 + GPU_DISPINFO_H);

    w &= ~7UL;

    if (w < GPU_MIN_DIM || h < GPU_MIN_DIM)
        return -1;
    if (w > QEMUVIRT_SCREEN_MAX_W || h > QEMUVIRT_SCREEN_MAX_H)
    {
        KINFO(("gpu: host asked for %ldx%ld, past the %dx%d we can hold\n",
               w, h, QEMUVIRT_SCREEN_MAX_W, QEMUVIRT_SCREEN_MAX_H));
        return -1;
    }

    *width = (WORD)w;
    *height = (WORD)h;
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
WORD virtio_gpu_init(WORD slot, ULONG fb, WORD *pwidth, WORD *pheight)
{
    WORD width = *pwidth;
    WORD height = *pheight;

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

    /*
     * Now that the queue is live, ask the host what size it wants. This
     * cannot happen before virtq_setup(): the query is an ordinary
     * command and there is nowhere to put it until then.
     */
    if (virtio_gpu_preferred(slot, &width, &height) == 0)
    {
        *pwidth = width;
        *pheight = height;
    }

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
    gpu_full_update = 1;
    gpu_active_frames = 10;
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
 * Colour table: every 16-bit VDI pixel to a finished ARGB longword.
 *
 * The VDI's truecolor pixel is RRRRRGGGGG0BBBBB -- 5:5:5 with bit 5
 * unused, not RGB565. Expansion replicates the high bits into the low
 * ones, (v << 3) | (v >> 2), so 0x1F maps to 0xFF and 0 maps to 0. The
 * naive v << 3 would map 0x1F to 0xF8, making white 248,248,248 --
 * wrong in a way that looks entirely correct until compared against a
 * reference.
 *
 * Building the table costs 65536 iterations once at mode-set and
 * removes the expansion, the byte order and the dead bit from the
 * per-pixel path entirely.
 */
void virtio_gpu_build_ctab(void)
{
    ULONG *tab = (ULONG *)QEMUVIRT_CTAB;
    ULONG i;

    for (i = 0; i < QEMUVIRT_CTAB_SIZE; i++)
    {
        ULONG r = (i >> 11) & 0x1f;
        ULONG g = (i >> 6) & 0x1f;    /* bit 5 is unused */
        ULONG b = i & 0x1f;

        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);

        /* X8R8G8B8: a big-endian longword store lays down X,R,G,B. */
        tab[i] = (r << 16) | (g << 8) | b;
    }
}

/*
 * Convert the VDI framebuffer into the ARGB surface and show it.
 *
 * Compare the 16-bit pixels with the last displayed frame. Unchanged
 * screens need no colour conversion, transfer or flush command.
 */
/* Input keeps the cursor on the 50 Hz path before pixels have changed. */
void virtio_gpu_activity(void)
{
    gpu_active_frames = 10;
}

void virtio_gpu_present(void)
{
    const ULONG *tab = (const ULONG *)QEMUVIRT_CTAB;
    const UWORD *src = (const UWORD *)v_bas_ad;
    ULONG *dst = (ULONG *)QEMUVIRT_ARGB_FB;
    GPU_DAMAGE damage;

    if (!gpu_ready || gpu_present_busy)
        return;

    /* Scan still screens at 10 Hz; resume 50 Hz immediately on input or
     * observed drawing. This avoids burning CPU on unchanged pixels. */
    if (!gpu_active_frames)
    {
        if (++gpu_idle_frames < 5)
            return;
    }
    else
        gpu_active_frames--;
    gpu_idle_frames = 0;

    gpu_present_busy = 1;

    if (gpu_damage_update(src, (UWORD *)QEMUVIRT_SHADOW_FB, dst, tab,
                          gpu_w, gpu_h, gpu_full_update, &damage))
    {
        virtio_gpu_flush(damage.x, damage.y, damage.w, damage.h);
        if (gpu_active_frames < 5)
            gpu_active_frames = 5;
    }
    gpu_full_update = 0;
    gpu_present_busy = 0;
}

#endif /* MACHINE_QEMU_VIRT */
