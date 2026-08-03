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

/*
 * Bytes actually written into p9_rx by the last successful p9_rpc().
 * "min" only guarantees a floor for indexing fixed offsets; callers
 * that must trust a variable-length field carried in the reply (e.g.
 * Rreaddir's count) need the real length to clamp against, since the
 * device is not trusted to report a count that fits what it sent.
 */
static ULONG p9_rx_len;

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
 * Returns 0 on a reply of the wanted type that is at least "min" bytes
 * long, or a negative GEMDOS error. An Rlerror is translated here so
 * callers never see an errno. A reply that is too short to safely
 * index -- whether it is a truncated Rlerror or a short "want" reply
 * that a caller will index at fixed offsets -- is EDRIVE, never a
 * silent read of stale buffer bytes.
 */
static LONG p9_rpc(ULONG len, UWORD tag, UBYTE want, ULONG min)
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

    p9_rx_len = (ULONG)got;

    if (p9_rx[4] == P9_RLERROR)
    {
        if (got < 11)
        {
            KINFO(("9p: short Rlerror (%ld)\n", got));
            return EDRIVE;
        }

        return p9_errno_to_gemdos(p9_ld32(p9_rx + 7));
    }

    if (p9_rx[4] != want)
    {
        KINFO(("9p: unexpected reply type %d, wanted %d\n",
               p9_rx[4], want));
        return EDRIVE;
    }

    if ((ULONG)got < min)
    {
        KINFO(("9p: short reply type %d (%ld < %lu)\n",
               p9_rx[4], got, min));
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

    rc = p9_rpc(off, 0xffff, P9_RVERSION, 11);   /* msize[4] at +7 */
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

    rc = p9_rpc(off, 0, P9_RATTACH, 7);   /* header only; qid unused */
    if (rc < 0)
        return rc;

    KINFO(("9p: attached, root fid %d\n", P9_ROOT_FID));
    p9_ready = 1;

    {
        UBYTE dirbuf[512];
        char name[64];
        ULONG off = 0, next = 0, pos;
        LONG n;

        if (p9_walk(P9_ROOT_FID, 1, 0) == 0 && p9_lopen(1, P9_O_RDONLY) == 0)
        {
            n = p9_readdir(1, 0, dirbuf, sizeof(dirbuf));
            for (pos = 0; n > 0 && pos < (ULONG)n; )
            {
                off = p9_dirent_next(dirbuf, (ULONG)n, pos, name,
                                      sizeof(name), &next);
                if (off == 0)
                    break;
                KINFO(("9p: dirent %s\n", name));
                pos = off;
            }
            p9_clunk(1);
        }
    }

    return 0;
}

LONG p9_walk(ULONG fid, ULONG newfid, const char *name)
{
    ULONG off = 7;

    p9_tx[4] = P9_TWALK;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, newfid);
    off += 4;

    if (name)
    {
        p9_st16(p9_tx + off, 1);
        off += 2;
        off = p9_put_str(p9_tx, off, name);
    }
    else
    {
        p9_st16(p9_tx + off, 0);    /* clone */
        off += 2;
    }

    return p9_rpc(off, 1, P9_RWALK, 7);   /* header only; wqid unused */
}

/*
 * Only the fields GEMDOS needs. The Rgetattr body is
 * valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] ...
 * so size starts at 7 + 8 + 13 + 4 + 4 + 4 + 8 + 8 = 56.
 */
LONG p9_getattr(ULONG fid, ULONG *size, UWORD *is_dir, ULONG *mtime,
                UWORD *writable)
{
    ULONG off = 7;
    ULONG mode;
    LONG rc;

    p9_tx[4] = P9_TGETATTR;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, P9_GETATTR_BASIC);
    off += 4;
    p9_st32(p9_tx + off, 0);        /* request_mask is 8 bytes */
    off += 4;

    /*
     * mtime_sec is the last field read, at +96, four bytes -- bytes
     * 96..99, so the reply must be at least 100 bytes long.
     */
    rc = p9_rpc(off, 1, P9_RGETATTR, 100);
    if (rc < 0)
        return rc;

    mode = p9_ld32(p9_rx + 28);     /* 7 + 8 + 13 */

    /* Low 32 bits of size are all a 16-bit-int guest can use anyway. */
    *size = p9_ld32(p9_rx + 56);
    *is_dir = (mode & 0170000UL) == 0040000UL;  /* S_IFDIR */
    *writable = (mode & 0200UL) != 0;           /* S_IWUSR */
    *mtime = p9_ld32(p9_rx + 96);               /* mtime_sec */

    return 0;
}

void p9_clunk(ULONG fid)
{
    ULONG off = 7;

    p9_tx[4] = P9_TCLUNK;
    p9_st32(p9_tx + off, fid);
    off += 4;

    p9_rpc(off, 1, P9_RCLUNK, 7);   /* header only */
}

/*
 * Rlopen is qid[13] iounit[4] starting at +7, but nothing here reads
 * either field -- only success/failure of the open matters -- so 7
 * (header only) is enough to pass as "min".
 */
LONG p9_lopen(ULONG fid, ULONG flags)
{
    ULONG off = 7;

    p9_tx[4] = P9_TLOPEN;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, flags);
    off += 4;

    return p9_rpc(off, 1, P9_RLOPEN, 7);
}

/*
 * Treaddir: fid[4] offset[8] count[4]. Rreaddir: count[4] data[count].
 *
 * count[4] sits at +7, so the reply must be at least 11 bytes before
 * it may be read at all -- that is what "min" enforces below. But
 * count itself comes from the device and is not trusted just because
 * it arrived: after reading it, this clamps against p9_rx_len (what
 * actually came back) before clamping against the caller's buffer, so
 * a device that claims more data than it sent can never make this
 * function copy stale or out-of-bounds bytes out of p9_rx.
 */
LONG p9_readdir(ULONG fid, ULONG offset, UBYTE *buf, ULONG buflen)
{
    ULONG off = 7;
    ULONG count, avail;
    LONG rc;

    p9_tx[4] = P9_TREADDIR;
    p9_st32(p9_tx + off, fid);
    off += 4;
    p9_st32(p9_tx + off, offset);   /* offset is 8 bytes, low half */
    off += 4;
    p9_st32(p9_tx + off, 0);
    off += 4;
    p9_st32(p9_tx + off, buflen);
    off += 4;

    rc = p9_rpc(off, 1, P9_RREADDIR, 11);   /* count[4] at +7 */
    if (rc < 0)
        return rc;

    count = p9_ld32(p9_rx + 7);

    /* Bytes actually available after the count field, per p9_rx_len. */
    avail = (p9_rx_len > 11) ? p9_rx_len - 11 : 0;
    if (count > avail)
        count = avail;
    if (count > buflen)
        count = buflen;

    memcpy(buf, p9_rx + 11, count);

    return (LONG)count;
}

/*
 * Step over one directory entry.
 *
 * Each is qid[13] offset[8] type[1] name[s], so the name length sits
 * at +22 and the name itself at +24. next_off receives the 9P offset
 * to hand to the following Treaddir, which is the entry's own offset
 * field rather than a byte count.
 *
 * buflen is the number of valid bytes in buf (what p9_readdir actually
 * returned), not the size of whatever array the caller declared it
 * in. Every offset used below is checked against that real length, so
 * a truncated or corrupt entry near the end of the data stops cleanly
 * instead of reading past it.
 */
ULONG p9_dirent_next(const UBYTE *buf, ULONG buflen, ULONG off, char *name,
                     ULONG namemax, ULONG *next_off)
{
    ULONG n;

    if (off + 24 > buflen)
        return 0;

    n = p9_ld16(buf + off + 22);
    if (n == 0 || n >= namemax)
        return 0;

    if (off + 24 + n > buflen)
        return 0;

    memcpy(name, buf + off + 24, n);
    name[n] = '\0';

    *next_off = p9_ld32(buf + off + 13);

    return off + 24 + n;
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
