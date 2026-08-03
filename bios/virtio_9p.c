/*
 * virtio_9p.c - 9P2000.L transport over virtio-mmio
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * 9P2000.L rather than 9P2000.u: Treaddir returns names directly and
 * Tgetattr returns a fixed layout, where .u needs variable-length stat
 * parsing for every operation.
 *
 * Every multi-byte field on the wire is little-endian and this is a
 * big-endian target, so fields are assembled byte by byte. Do not be
 * tempted to overlay a struct.
 */

#include "emutos.h"
#include "virtio.h"
#include "virtio_9p.h"
#include "qemuvirt.h"
#include "gemerror.h"
#include "string.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

#define P9_QSIZE 8

static WORD p9_slot = -1;
static WORD p9_ready;
static VIRTQ p9_vq;

static UBYTE p9_ring[2048] __attribute__((aligned(16)));
static UBYTE p9_tx[P9_MSIZE] __attribute__((aligned(4)));
static UBYTE p9_rx[P9_MSIZE] __attribute__((aligned(4)));

/* Little-endian stores. */
static void p9_st16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)(v >> 8);
}

static void p9_st32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)((v >> 8) & 0xff);
    p[2] = (UBYTE)((v >> 16) & 0xff);
    p[3] = (UBYTE)((v >> 24) & 0xff);
}

/* Little-endian loads. */
static UWORD p9_ld16(const UBYTE *p)
{
    return (UWORD)(p[0] | ((UWORD)p[1] << 8));
}

static ULONG p9_ld32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* Append a 9P string: len[2] followed by the bytes. Returns new offset. */
static ULONG p9_put_str(UBYTE *buf, ULONG off, const char *s)
{
    ULONG n = 0;

    while (s[n])
        n++;

    p9_st16(buf + off, (UWORD)n);
    off += 2;
    memcpy(buf + off, s, n);

    return off + n;
}

/*
 * Linux errno as sent by the host, mapped to GEMDOS. Kept here so the
 * GEMDOS layer above never sees an errno.
 */
LONG p9_errno_to_gemdos(ULONG err)
{
    switch (err)
    {
    case 2:     return EFILNF;      /* ENOENT */
    case 20:    return EPTHNF;      /* ENOTDIR */
    case 1:                         /* EPERM */
    case 13:                        /* EACCES */
    case 30:    return EACCDN;      /* EROFS */
    case 23:                        /* ENFILE */
    case 24:    return ENHNDL;      /* EMFILE */
    case 28:    return ENSMEM;      /* ENOSPC */
    default:    return EACCDN;
    }
}

/*
 * Send the message built in p9_tx and collect the reply in p9_rx.
 *
 * Returns the reply type, or a negative GEMDOS error. An Rlerror is
 * translated here so callers never see an errno.
 */
static LONG p9_rpc(ULONG len, UWORD tag, UBYTE want)
{
    LONG got;

    /* Caller has already set p9_tx[4], the message type. */
    p9_st32(p9_tx, len);
    p9_st16(p9_tx + 5, tag);

    got = virtq_request(&p9_vq, p9_tx, len, p9_rx, P9_MSIZE);
    if (got < 7)
    {
        KINFO(("9p: no reply (%ld)\n", got));
        return EDRIVE;
    }

    if (p9_rx[4] == P9_RLERROR)
        return p9_errno_to_gemdos(p9_ld32(p9_rx + 7));

    if (p9_rx[4] != want)
    {
        KINFO(("9p: unexpected reply type %d, wanted %d\n",
               p9_rx[4], want));
        return EDRIVE;
    }

    return 0;
}

LONG p9_attach(void)
{
    ULONG off;
    LONG rc;

    /* Tversion: msize[4] version[s] */
    off = 7;
    p9_tx[4] = P9_TVERSION;
    p9_st32(p9_tx + off, P9_MSIZE);
    off += 4;
    off = p9_put_str(p9_tx, off, "9P2000.L");

    rc = p9_rpc(off, 0xffff, P9_RVERSION);
    if (rc < 0)
        return rc;

    KINFO(("9p: slot %d msize %ld version 9P2000.L\n",
           p9_slot, p9_ld32(p9_rx + 7)));

    /* Tattach: fid[4] afid[4] uname[s] aname[s] n_uname[4] */
    off = 7;
    p9_tx[4] = P9_TATTACH;
    p9_st32(p9_tx + off, P9_ROOT_FID);
    off += 4;
    p9_st32(p9_tx + off, 0xffffffffUL);     /* afid: NOFID */
    off += 4;
    off = p9_put_str(p9_tx, off, "root");
    off = p9_put_str(p9_tx, off, "");
    p9_st32(p9_tx + off, 0);                /* n_uname */
    off += 4;

    rc = p9_rpc(off, 0, P9_RATTACH);
    if (rc < 0)
        return rc;

    KINFO(("9p: attached, root fid %d\n", P9_ROOT_FID));
    p9_ready = 1;

    return 0;
}

void virtio_9p_attach(WORD slot)
{
    p9_slot = slot;

    if (virtq_setup(&p9_vq, slot, 0, P9_QSIZE, p9_ring) != 0)
    {
        KINFO(("9p: queue setup failed\n"));
        p9_slot = -1;
        return;
    }

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    p9_attach();
}

WORD virtio_9p_present(void)
{
    return p9_ready;
}

#endif /* MACHINE_QEMU_VIRT */
