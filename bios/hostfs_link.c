/*
 * hostfs_link.c - host folder transport over virtio-serial
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * WHY VIRTIO-SERIAL AND NOT 9P
 *
 * QEMU cannot build virtio-9p on Windows at all -- its meson.build
 * requires the host to be Linux, macOS or FreeBSD -- and Windows is a
 * primary target.  virtio-serial is a plain byte pipe present in every
 * QEMU build with both socket and pipe backends, so it cannot be
 * absent, deprecated or experimental.  The filesystem work moves to an
 * ordinary host process (tools/hostfsd) which is the same program on
 * all three platforms; this file is the wire to it.
 *
 * THE FRAME
 *
 * hostfs_proto.h holds the layout, shared verbatim with the helper.
 * One request, one reply, strictly alternating: the guest is
 * single-threaded through osif() and cannot have two calls outstanding,
 * so there is no request id to match up.
 *
 * Every field is little-endian and this is a big-endian target, so
 * fields are assembled and read byte by byte.  Do not be tempted to
 * overlay a struct over the buffer -- it compiles, and it reads
 * plausible-looking garbage.
 *
 * `int` IS 16 BITS HERE (-mshort).  Frame lengths reach 4096 and file
 * offsets reach a filesystem's worth, so nothing below counts or
 * offsets in an int.
 *
 * THE PORT HANDSHAKE
 *
 * This is the one genuinely new piece.  A virtio-console device with
 * VIRTIO_CONSOLE_F_MULTIPORT has, besides port 0's pair, a control
 * queue pair at 2 and 3, and port N's data queues at 2N+2 and 2N+3.
 * QEMU reserves port id 0 for a `virtconsole`, so a `virtserialport`
 * lands at id 1 and its queues are 4 and 5 -- which is why the
 * multiport handshake is done rather than skipped: without it the
 * device offers only port 0, and the port we were given is not it.
 *
 * The handshake is: negotiate MULTIPORT, bring up the control queues,
 * DRIVER_OK, send DEVICE_READY, take a DEVICE_ADD for each port that
 * exists, answer each with PORT_READY, then send PORT_OPEN for the one
 * we mean to use.  Until that last message QEMU's virtio_serial_guest_
 * ready() refuses to hand us anything the host wrote, so a driver that
 * skips it sees a channel that accepts writes and never answers.
 *
 * The host's own PORT_OPEN tells us the far end of the chardev is
 * connected.  It is waited for, briefly, because the launcher may start
 * the helper at about the same moment as QEMU: data written while the
 * host end is disconnected is DISCARDED by QEMU, not queued, so sending
 * HELLO too early loses it silently.
 *
 * FAILURE
 *
 * The guest must never hang and must never guess.  Every wait is
 * bounded and expiry is EDRIVE.  A reply whose length or shape is wrong
 * means the byte stream is no longer aligned to frames, and there is no
 * way to find the next boundary; the link is marked dead and every
 * later call fails with EDRIVE rather than risk answering one request
 * with another's data.  A drive that stops working is diagnosable; one
 * that returns the wrong file is not.
 *
 * With no device at all, hostfs_link_present() stays zero and the
 * GEMDOS layer simply does not register drive C.  The feature is
 * absent, not broken.
 */

#include "emutos.h"
#include "virtio.h"
#include "hostfs_link.h"
#include "hostfs_proto.h"
#include "gemerror.h"
#include "string.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

/*
 * The GEMDOS layer takes the wire's limits from hostfs_link.h, because
 * it cannot include hostfs_proto.h without clobbering its own
 * HOSTFS_MAX_PATH.  This is the one place both headers are visible, so
 * it is where the two are proved to agree -- a negative array size is
 * a compile error, not a runtime surprise.
 */
typedef char hostfs_link_assert_limits[
    (HOSTFS_LINK_MAX_PATH == HOSTFS_MAX_PATH
     && HOSTFS_LINK_O_RDONLY == HOSTFS_O_RDONLY
     && HOSTFS_LINK_O_WRONLY == HOSTFS_O_WRONLY
     && HOSTFS_LINK_O_RDWR == HOSTFS_O_RDWR) ? 1 : -1];

/* Device configuration space: cols[2] rows[2] max_nr_ports[4] emerg_wr[4] */
#define VIRTIO_CONSOLE_CFG_MAX_PORTS    4

/* Control events -- virtio spec, "Device Operation: Multiport". */
#define VIRTIO_CONSOLE_DEVICE_READY     0
#define VIRTIO_CONSOLE_DEVICE_ADD       1
#define VIRTIO_CONSOLE_DEVICE_REMOVE    2
#define VIRTIO_CONSOLE_PORT_READY       3
#define VIRTIO_CONSOLE_CONSOLE_PORT     4
#define VIRTIO_CONSOLE_RESIZE           5
#define VIRTIO_CONSOLE_PORT_OPEN        6
#define VIRTIO_CONSOLE_PORT_NAME        7

#define VIRTIO_CONSOLE_BAD_ID           0xffffffffUL

#define HL_CTRL_HDR     8               /* id[4] event[2] value[2] */

/*
 * Queue map.  Port 0 is the odd one out: it keeps queues 0 and 1, and
 * the control pair sits between it and every other port.
 */
#define HL_VQ_CTRL_RX   2
#define HL_VQ_CTRL_TX   3
#define HL_VQ_PORT_RX(id)   ((WORD)((id) == 0 ? 0 : 2 + 2 * (id)))
#define HL_VQ_PORT_TX(id)   ((WORD)((id) == 0 ? 1 : 3 + 2 * (id)))

/*
 * Ports this driver is willing to look at.  QEMU's default max_nr_ports
 * is 31, but a launcher that needed more than a couple of serial ports
 * would be doing something this drive has no part in; four covers the
 * reserved console slot plus room for a launcher to add its own before
 * ours.  A port above this is reported rather than silently ignored.
 */
#define HL_MAX_PORTS    4

#define HL_QSIZE        8
/* desc 16*8=128, avail 4+16, used 4+64, each 16-aligned: 256 is ample. */
#define HL_RING_BYTES   256
#define HL_NRINGS       (2 + 2 * HL_MAX_PORTS)

#define HL_CTRL_BUFS    4
#define HL_CTRL_BUFSZ   64

/*
 * The name a launcher gives the port with `virtserialport,name=...`.
 * Matching it is what keeps this driver off a virtio-serial port that
 * belongs to something else; when no port carries it, the first usable
 * port is tried and HELLO decides.
 */
#define HL_PORT_NAME    "shinogi.hostfs"

/*
 * Spin bounds.
 *
 * VIRTQ_SPIN_LIMIT is what the other drivers use for a request and is
 * reused for the data path.  The control path has its own, entered
 * HL_OPEN_ROUNDS times over, and together they are how long the guest
 * will wait for the HELPER TO CONNECT.
 *
 * That wait is not padding.  QEMU's chardev is the listener, so the
 * helper cannot connect until QEMU has bound the socket -- which is
 * after the launcher has started QEMU -- while the guest probes virtio
 * within the first moments of boot.  The two really do race, and
 * anything the guest writes before the far end attaches is DISCARDED by
 * QEMU rather than queued, so losing the race loses the drive for the
 * whole session.  These numbers give the launcher a few seconds of
 * slack; measured, 16 rounds of 500000 was under a second and a helper
 * started one second late already lost.
 *
 * The cost of being generous is paid only when a virtio-serial device
 * is present and no helper ever appears: one pause at boot, once.  With
 * no device at all -- the ordinary "feature not wanted" case -- none of
 * this runs, because virtio_probe() never calls attach.
 */
#define HL_CTRL_SPIN    2000000L
#define HL_OPEN_ROUNDS  32      /* control rounds waiting for the host end */

/*
 * How long a reply may take.
 *
 * Once the drive is up this is the bound every other driver uses.
 * During attach it is deliberately far shorter: a machine with no
 * helper answers HELLO with silence, and paying the full bound for that
 * is a visible pause on every boot for a feature that is merely absent.
 */
#define HL_HELLO_SPIN   2000000L

static WORD hl_slot = -1;
static WORD hl_ready;           /* HELLO answered: the drive may be used */
static WORD hl_dead;            /* channel desynchronised: EDRIVE for ever */
static WORD hl_port = -1;
static LONG hl_rx_spin = VIRTQ_SPIN_LIMIT;

static VIRTQ hl_cvq_rx, hl_cvq_tx;      /* control */
static VIRTQ hl_pvq[HL_MAX_PORTS][2];   /* every port's rx/tx, set up once */
static VIRTQ hl_vq_rx, hl_vq_tx;        /* the port we settled on */

static UBYTE hl_rings[HL_NRINGS][HL_RING_BYTES] __attribute__((aligned(16)));

static UBYTE hl_ctrl_rx[HL_CTRL_BUFS][HL_CTRL_BUFSZ] __attribute__((aligned(4)));
static UBYTE hl_ctrl_tx[HL_CTRL_HDR] __attribute__((aligned(4)));

static UBYTE hl_req[HOSTFS_MAX_FRAME] __attribute__((aligned(4)));
static UBYTE hl_rep[HOSTFS_MAX_FRAME] __attribute__((aligned(4)));
static UBYTE hl_rx[HOSTFS_MAX_FRAME] __attribute__((aligned(4)));

/* Bytes of hl_rep that the last successful hl_rpc() actually received. */
static ULONG hl_rep_len;
static WORD hl_batch_enabled;
static UBYTE hl_batch[HOSTFS_MAX_REP_DATA];
static ULONG hl_batch_handle, hl_batch_cookie;
static ULONG hl_batch_pos, hl_batch_len;

/* Little-endian stores and loads, byte by byte.  See the file header. */
static void hl_st16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)(v >> 8);
}

static void hl_st32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)((v >> 8) & 0xff);
    p[2] = (UBYTE)((v >> 16) & 0xff);
    p[3] = (UBYTE)((v >> 24) & 0xff);
}

static UWORD hl_ld16(const UBYTE *p)
{
    return (UWORD)(p[0] | ((UWORD)p[1] << 8));
}

static ULONG hl_ld32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/*
 * Declare the channel unusable.
 *
 * Called for anything that means the byte stream is no longer aligned
 * to frame boundaries: a length outside the legal range, a frame that
 * overruns the buffer, bytes arriving past the end of a reply, or a
 * wait that expired with a request outstanding.  There is deliberately
 * no way back: resynchronising means guessing where the next frame
 * starts, and a wrong guess pairs one request with another's reply,
 * which reaches the caller as the contents of a different file.
 */
static void hl_kill(const char *why)
{
    if (!hl_dead)
        KINFO(("hostfs-link: channel dead (%s)\n", why));
    hl_dead = 1;
    hl_ready = 0;
}

/* Bounded wait on one queue.  Returns the head index, or -1. */
static WORD hl_wait(VIRTQ *vq, ULONG *len, LONG spin)
{
    LONG i;

    for (i = 0; i < spin; i++)
    {
        WORD head = virtq_poll(vq, len);
        if (head >= 0)
            return head;
    }

    return -1;
}

/*
 * ===========================================================================
 * The control queue: bringing a port up
 * ===========================================================================
 */

static void hl_ctrl_post(WORD i)
{
    virtq_add(&hl_cvq_rx, i, (ULONG)hl_ctrl_rx[i], HL_CTRL_BUFSZ,
              VRING_DESC_F_WRITE);
    virtq_kick(&hl_cvq_rx, i);
}

static WORD hl_ctrl_send(ULONG id, UWORD event, UWORD value)
{
    hl_st32(hl_ctrl_tx, id);
    hl_st16(hl_ctrl_tx + 4, event);
    hl_st16(hl_ctrl_tx + 6, value);

    virtq_add(&hl_cvq_tx, 0, (ULONG)hl_ctrl_tx, (ULONG)HL_CTRL_HDR, 0);
    virtq_kick(&hl_cvq_tx, 0);

    return hl_wait(&hl_cvq_tx, NULL, HL_CTRL_SPIN);
}

/* Per-port state gathered from the control queue during attach. */
static UBYTE hl_port_present[HL_MAX_PORTS];
static UBYTE hl_port_console[HL_MAX_PORTS];
static UBYTE hl_port_named[HL_MAX_PORTS];
static UBYTE hl_port_hostopen[HL_MAX_PORTS];

/*
 * Does the PORT_NAME payload spell our name?
 *
 * The name is the frame's tail and QEMU sends it NUL-terminated, but
 * the spec does not require that, so the terminator is tolerated rather
 * than relied on.
 */
static WORD hl_name_matches(const UBYTE *p, ULONG len)
{
    ULONG want = (ULONG)strlen(HL_PORT_NAME);
    ULONG i;

    while (len > 0 && p[len - 1] == '\0')
        len--;

    if (len != want)
        return 0;

    for (i = 0; i < want; i++)
        if (p[i] != (UBYTE)HL_PORT_NAME[i])
            return 0;

    return 1;
}

/*
 * Drain whatever the device has to say, answering DEVICE_ADD as it
 * arrives.  Returns the number of messages handled.
 */
static WORD hl_ctrl_pump(void)
{
    WORD handled = 0;
    WORD head;
    ULONG len = 0;

    while ((head = hl_wait(&hl_cvq_rx, &len, HL_CTRL_SPIN)) >= 0)
    {
        const UBYTE *m;
        ULONG id;
        UWORD event, value;

        if (head >= HL_CTRL_BUFS)
        {
            KINFO(("hostfs-link: control head %d out of range\n", head));
            break;
        }

        m = hl_ctrl_rx[head];
        handled++;

        /*
         * A control message shorter than its own header tells us
         * nothing, and indexing it would read whatever the last message
         * left in the buffer.  Drop it and re-post; the control queue
         * is message-framed by the device, so one bad message does not
         * desynchronise anything.
         */
        if (len < (ULONG)HL_CTRL_HDR)
        {
            hl_ctrl_post(head);
            continue;
        }

        id = hl_ld32(m);
        event = hl_ld16(m + 4);
        value = hl_ld16(m + 6);

        if (id >= (ULONG)HL_MAX_PORTS)
        {
            /* Not ours to drive, but say so: a port we cannot reach is
             * the difference between "no helper" and "wrong slot". */
            if (event == VIRTIO_CONSOLE_DEVICE_ADD)
                KINFO(("hostfs-link: port %lu above the %d this driver "
                       "handles\n", id, (WORD)HL_MAX_PORTS));
            hl_ctrl_post(head);
            continue;
        }

        switch (event)
        {
        case VIRTIO_CONSOLE_DEVICE_ADD:
            hl_port_present[id] = 1;
            /*
             * PORT_READY is what makes the device tell us the port's
             * name and whether the host end is connected.  It must be
             * sent for every port, not only the one we want, or a
             * device with several ports stops enumerating.
             */
            hl_ctrl_send(id, VIRTIO_CONSOLE_PORT_READY, 1);
            break;

        case VIRTIO_CONSOLE_CONSOLE_PORT:
            hl_port_console[id] = 1;
            break;

        case VIRTIO_CONSOLE_PORT_NAME:
            if (hl_name_matches(m + HL_CTRL_HDR, len - HL_CTRL_HDR))
                hl_port_named[id] = 1;
            break;

        case VIRTIO_CONSOLE_PORT_OPEN:
            hl_port_hostopen[id] = value ? 1 : 0;
            break;

        case VIRTIO_CONSOLE_DEVICE_REMOVE:
            hl_port_present[id] = 0;
            break;

        default:
            break;              /* RESIZE and anything newer */
        }

        hl_ctrl_post(head);
    }

    return handled;
}

/*
 * Which port to talk to.
 *
 * A name match wins outright.  Failing that the first non-console port
 * is taken, since a console port is somebody's terminal and writing a
 * request frame into it would be both useless and visible.  HELLO is
 * the final arbiter: if the choice was wrong there is no reply and the
 * drive is not registered, which is the same outcome as no helper.
 */
static WORD hl_pick_port(void)
{
    WORD i;

    for (i = 0; i < HL_MAX_PORTS; i++)
        if (hl_port_present[i] && hl_port_named[i])
            return i;

    for (i = 0; i < HL_MAX_PORTS; i++)
        if (hl_port_present[i] && !hl_port_console[i])
            return i;

    return -1;
}

/*
 * ===========================================================================
 * The data path
 * ===========================================================================
 */

/*
 * Hand the device one request frame.
 *
 * A completion here says the device took the bytes, NOT that the host
 * received them: with the far end of the chardev disconnected QEMU
 * discards the data and still completes the descriptor.  That is why
 * the reply wait, not this one, is what decides whether the helper is
 * there.
 */
static WORD hl_send(ULONG len)
{
    virtq_add(&hl_vq_tx, 0, (ULONG)hl_req, len, 0);
    virtq_kick(&hl_vq_tx, 0);

    return hl_wait(&hl_vq_tx, NULL, VIRTQ_SPIN_LIMIT);
}

/*
 * Collect one reply frame into hl_rep.
 *
 * A byte pipe is not message-framed: the helper's reply arrives in
 * however many pieces the socket chose to deliver it in, so the header
 * is reassembled before its `len` can be read and the rest is gathered
 * against that length.  Nothing is indexed until enough bytes have
 * actually arrived to hold it.
 *
 * `len` is the peer's claim and is checked against the legal range and
 * against our buffer BEFORE it decides how much more to wait for.  A
 * frame that is followed by bytes we did not ask for is a
 * desynchronised channel, not a fast peer: the protocol is strictly one
 * reply per request.
 *
 * Returns 0, or a negative GEMDOS error.
 */
static LONG hl_recv(void)
{
    ULONG got = 0;
    ULONG want = (ULONG)HOSTFS_REP_HDR;

    hl_rep_len = 0;

    while (got < want)
    {
        ULONG n = 0;
        WORD head;

        virtq_add(&hl_vq_rx, 0, (ULONG)hl_rx, (ULONG)HOSTFS_MAX_FRAME,
                  VRING_DESC_F_WRITE);
        virtq_kick(&hl_vq_rx, 0);

        head = hl_wait(&hl_vq_rx, &n, hl_rx_spin);
        if (head < 0)
        {
            /*
             * Nothing came back.  A request has already gone out, so
             * the reply may still be in flight and would be read as the
             * head of the NEXT call's reply.  There is no safe way to
             * carry on from here.
             */
            hl_kill("no reply");
            return EDRIVE;
        }

        if (n == 0)
            continue;           /* an empty completion: ask again */

        if (n > (ULONG)HOSTFS_MAX_FRAME || got + n > (ULONG)HOSTFS_MAX_FRAME)
        {
            hl_kill("reply overruns the buffer");
            return EDRIVE;
        }

        memcpy(hl_rep + got, hl_rx, n);
        got += n;

        if (got >= (ULONG)HOSTFS_REP_HDR && want == (ULONG)HOSTFS_REP_HDR)
        {
            want = (ULONG)hl_ld16(hl_rep);

            if (want < (ULONG)HOSTFS_REP_HDR || want > (ULONG)HOSTFS_MAX_FRAME)
            {
                hl_kill("illegal reply length");
                return EDRIVE;
            }
        }

        if (got > want)
        {
            hl_kill("bytes past the end of a reply");
            return EDRIVE;
        }
    }

    hl_rep_len = got;

    return E_OK;
}

/*
 * One request, one reply.
 *
 * Returns E_OK with hl_rep/hl_rep_len holding the reply, or a negative
 * GEMDOS error.  The helper owns the errno-to-GEMDOS mapping, so a
 * status other than 0 is passed through as it stands -- with one check,
 * that it really is negative: a positive `status` is not a GEMDOS error
 * and a caller must never mistake one for success.
 */
static LONG hl_rpc(UWORD op, ULONG a, ULONG b, ULONG c,
                   const void *data, ULONG dlen)
{
    ULONG len = (ULONG)HOSTFS_REQ_HDR + dlen;
    LONG rc;
    LONG status;

    if (hl_dead || hl_slot < 0)
        return EDRIVE;

    if (dlen > (ULONG)HOSTFS_MAX_REQ_DATA)
        return EPTHNF;          /* nothing this long is a real path */

    hl_st16(hl_req, (UWORD)len);
    hl_st16(hl_req + 2, op);
    hl_st32(hl_req + 4, a);
    hl_st32(hl_req + 8, b);
    hl_st32(hl_req + 12, c);
    if (dlen)
        memcpy(hl_req + HOSTFS_REQ_HDR, data, dlen);

    if (hl_send(len) < 0)
    {
        hl_kill("send timed out");
        return EDRIVE;
    }

    rc = hl_recv();
    if (rc < 0)
        return rc;

    /* A GEMDOS error is a small negative number carried in 16 bits. */
    status = (LONG)(WORD)hl_ld16(hl_rep + 2);
    if (status > 0)
    {
        hl_kill("reply status is not a GEMDOS error");
        return EDRIVE;
    }

    return status;
}

/*
 * ===========================================================================
 * Operations
 * ===========================================================================
 */

/* Every path is measured here so no caller has to remember to. */
static ULONG hl_pathlen(const char *path)
{
    return (ULONG)strlen(path);
}

LONG hostfs_link_stat(const char *path, ULONG *size, UWORD *is_dir,
                      ULONG *mtime, UWORD *writable)
{
    UBYTE attr;
    LONG rc;

    rc = hl_rpc(HOSTFS_OP_STAT, 0, 0, 0, path, hl_pathlen(path));
    if (rc < 0)
        return rc;

    /* The attribute byte is data[0], so a reply without it is short. */
    if (hl_rep_len < (ULONG)HOSTFS_REP_HDR + 1)
    {
        hl_kill("short STAT reply");
        return EDRIVE;
    }

    attr = hl_rep[HOSTFS_REP_HDR];

    *size = hl_ld32(hl_rep + 4);
    *mtime = hl_ld32(hl_rep + 8);
    *is_dir = (attr & HOSTFS_FA_SUBDIR) ? 1 : 0;
    *writable = (attr & HOSTFS_FA_RO) ? 0 : 1;

    return E_OK;
}

LONG hostfs_link_open(const char *path, UWORD mode)
{
    LONG rc = hl_rpc(HOSTFS_OP_OPEN, (ULONG)mode, 0, 0, path,
                     hl_pathlen(path));

    if (rc < 0)
        return rc;

    return (LONG)hl_ld32(hl_rep + 4);
}

LONG hostfs_link_close(ULONG handle)
{
    return hl_rpc(HOSTFS_OP_CLOSE, handle, 0, 0, NULL, 0);
}

/*
 * Read, chunked.
 *
 * A frame carries at most HOSTFS_MAX_REP_DATA bytes, and GEMDOS Fread
 * asks for whatever the caller asked for -- 16KB in the self-test.  The
 * loop is here rather than in the GEMDOS layer so that a short return
 * means end of file and nothing else, which is what ixread()'s callers
 * already assume.
 *
 * count and offset are ULONG throughout: with -mshort an int here wraps
 * at 32767, which is a third of the way into one self-test buffer.
 */
LONG hostfs_link_read(ULONG handle, ULONG offset, UBYTE *buf, ULONG count)
{
    ULONG done = 0;

    while (done < count)
    {
        ULONG chunk = count - done;
        ULONG payload;
        ULONG got;
        LONG rc;

        if (chunk > (ULONG)HOSTFS_MAX_REP_DATA)
            chunk = (ULONG)HOSTFS_MAX_REP_DATA;

        rc = hl_rpc(HOSTFS_OP_READ, handle, offset + done, chunk, NULL, 0);
        if (rc < 0)
            return done ? (LONG)done : rc;

        /*
         * Two independent claims arrive here: `a`, the count the helper
         * says it read, and the frame length, which is what actually
         * turned up.  Neither is trusted against the other -- a count
         * larger than the payload would copy whatever the previous
         * reply left in the buffer, and a count larger than the chunk
         * would run past the caller's buffer.
         */
        payload = hl_rep_len - (ULONG)HOSTFS_REP_HDR;
        got = hl_ld32(hl_rep + 4);

        if (got > payload || got > chunk)
        {
            hl_kill("READ reply claims more than it carries");
            return done ? (LONG)done : EDRIVE;
        }

        if (got == 0)
            break;              /* end of file */

        memcpy(buf + done, hl_rep + HOSTFS_REP_HDR, got);
        done += got;

        if (got < chunk)
            break;              /* short read: end of file */
    }

    return (LONG)done;
}

/*
 * Write, chunked.
 *
 * The mirror image of the read loop above, and it exists for the same
 * reason: a frame carries at most HOSTFS_MAX_REQ_DATA bytes of payload,
 * while GEMDOS Fwrite is handed whatever the application had -- tens of
 * kilobytes at a time from a file copy.  A write that issued one frame
 * and reported the caller's own count back would truncate the file on
 * the host and tell the guest it had succeeded, which is the single
 * worst failure this drive could have.
 *
 * The offset is explicit, as it is for READ, so the helper never has to
 * remember where a handle was left: the guest's own file position is
 * the only cursor, and one request cannot be spoiled by another.
 *
 * count and offset are ULONG throughout: with -mshort an int here wraps
 * at 32767, a quarter of the way into one self-test buffer.
 */
LONG hostfs_link_write(ULONG handle, ULONG offset, const UBYTE *buf,
                       ULONG count)
{
    ULONG done = 0;

    while (done < count)
    {
        ULONG chunk = count - done;
        ULONG put;
        LONG rc;

        if (chunk > (ULONG)HOSTFS_MAX_REQ_DATA)
            chunk = (ULONG)HOSTFS_MAX_REQ_DATA;

        rc = hl_rpc(HOSTFS_OP_WRITE, handle, offset + done, 0,
                    buf + done, chunk);
        if (rc < 0)
            return done ? (LONG)done : rc;

        /*
         * The count is the helper's claim, and it is checked against
         * what the helper was actually given.  A larger one is
         * impossible for an honest helper and would make this loop skip
         * bytes it never sent -- the file would be short by exactly the
         * difference and nothing would say so.
         */
        put = hl_ld32(hl_rep + 4);
        if (put > chunk)
        {
            hl_kill("WRITE reply claims more than it was given");
            return done ? (LONG)done : EDRIVE;
        }

        done += put;

        /*
         * A short write is the host declining to take more -- a full
         * filesystem, a quota.  Stopping here and reporting the count
         * is what GEMDOS callers already handle; going round again
         * would spin against a disk that is not going to empty.
         */
        if (put < chunk)
            break;
    }

    return (LONG)done;
}

LONG hostfs_link_create(const char *path, UWORD attr)
{
    LONG rc = hl_rpc(HOSTFS_OP_CREATE, (ULONG)attr, 0, 0, path,
                     hl_pathlen(path));

    if (rc < 0)
        return rc;

    return (LONG)hl_ld32(hl_rep + 4);
}

LONG hostfs_link_delete(const char *path)
{
    return hl_rpc(HOSTFS_OP_DELETE, 0, 0, 0, path, hl_pathlen(path));
}

/*
 * RENAME is the one request carrying two paths, separated by a NUL that
 * is part of the payload rather than a terminator.  They are assembled
 * here, in one buffer, so the wire format has exactly one writer.
 *
 * Static, not a local: this runs on the BDOS's small private stack (see
 * the note on dirent_name in bdos/hostfs.c), which two kilobytes of
 * path would not survive.
 */
static char hl_pair[2 * HOSTFS_MAX_PATH + 1];

LONG hostfs_link_rename(const char *from, const char *to)
{
    ULONG nf = hl_pathlen(from);
    ULONG nt = hl_pathlen(to);

    if (nf + 1 + nt > (ULONG)sizeof(hl_pair))
        return EPTHNF;          /* nothing this long is a real pair of paths */

    memcpy(hl_pair, from, nf);
    hl_pair[nf] = '\0';
    memcpy(hl_pair + nf + 1, to, nt);

    return hl_rpc(HOSTFS_OP_RENAME, 0, 0, 0, hl_pair, nf + 1 + nt);
}

LONG hostfs_link_mkdir(const char *path)
{
    return hl_rpc(HOSTFS_OP_MKDIR, 0, 0, 0, path, hl_pathlen(path));
}

LONG hostfs_link_rmdir(const char *path)
{
    return hl_rpc(HOSTFS_OP_RMDIR, 0, 0, 0, path, hl_pathlen(path));
}

LONG hostfs_link_setattr(const char *path, UWORD attr)
{
    return hl_rpc(HOSTFS_OP_SETATTR, (ULONG)attr, 0, 0, path,
                  hl_pathlen(path));
}

LONG hostfs_link_opendir(const char *path)
{
    LONG rc = hl_rpc(HOSTFS_OP_OPENDIR, 0, 0, 0, path, hl_pathlen(path));

    if (rc < 0)
        return rc;

    return (LONG)hl_ld32(hl_rep + 4);
}

/*
 * One directory entry.
 *
 * The cookie is OPAQUE and is carried whole.  The 9P transport batched
 * a page of entries and kept only the low half of an eight-byte cookie,
 * which is harmless on tmpfs (small integers) and fatal on ext4 (a hash
 * whose identifying half lives in the HIGH word): every seek landed
 * back at the start and the directory repeated its first page for ever.
 * One entry per call cannot fail that way, and nothing here does
 * arithmetic on the value.
 */
static LONG hl_readdir_batch(ULONG handle, ULONG cookie, char *name,
                             ULONG namemax, ULONG *next)
{
    ULONG length;

    if (hl_batch_handle != handle || hl_batch_cookie != cookie ||
        hl_batch_pos >= hl_batch_len)
    {
        ULONG offset = 0, count, entry, previous = cookie;
        LONG rc;

        hl_batch_len = hl_batch_pos = 0;
        rc = hl_rpc(HOSTFS_OP_READDIR_BATCH, handle, cookie, 0, NULL, 0);
        if (rc < 0) return rc;
        length = hl_rep_len - (ULONG)HOSTFS_REP_HDR;
        count = hl_ld32(hl_rep + 8);
        if (!count || count > length / 7)
        {
            hl_kill("invalid directory batch count");
            return EDRIVE;
        }
        for (entry = 0; entry < count; entry++)
        {
            ULONG cookie_value, n, i;
            if (offset + 6 > length)
            {
                hl_kill("short directory batch entry");
                return EDRIVE;
            }
            cookie_value = hl_ld32(hl_rep + HOSTFS_REP_HDR + offset);
            n = hl_ld16(hl_rep + HOSTFS_REP_HDR + offset + 4);
            if (!n || n > (ULONG)HOSTFS_MAX_NAME || offset + 6 + n > length ||
                cookie_value == previous)
            {
                hl_kill("invalid directory batch entry");
                return EDRIVE;
            }
            for (i = 0; i < n; i++)
                if (!hl_rep[HOSTFS_REP_HDR + offset + 6 + i])
                {
                    hl_kill("NUL in directory batch name");
                    return EDRIVE;
                }
            previous = cookie_value;
            offset += 6 + n;
        }
        if (offset != length || previous != hl_ld32(hl_rep + 4))
        {
            hl_kill("inconsistent directory batch tail");
            return EDRIVE;
        }
        memcpy(hl_batch, hl_rep + HOSTFS_REP_HDR, length);
        hl_batch_handle = handle;
        hl_batch_cookie = cookie;
        hl_batch_len = length;
    }

    *next = hl_ld32(hl_batch + hl_batch_pos);
    length = hl_ld16(hl_batch + hl_batch_pos + 4);
    if (length < namemax)
    {
        memcpy(name, hl_batch + hl_batch_pos + 6, length);
        name[length] = '\0';
    }
    hl_batch_pos += 6 + length;
    hl_batch_cookie = *next;
    return E_OK;
}

LONG hostfs_link_readdir(ULONG handle, ULONG cookie, char *name,
                         ULONG namemax, ULONG *next)
{
    ULONG nlen;
    LONG rc;

    name[0] = '\0';
    *next = cookie;

    if (hl_batch_enabled)
        return hl_readdir_batch(handle, cookie, name, namemax, next);

    rc = hl_rpc(HOSTFS_OP_READDIR, handle, cookie, 0, NULL, 0);
    if (rc < 0)
        return rc;              /* ENMFIL ends the enumeration */

    nlen = hl_rep_len - (ULONG)HOSTFS_REP_HDR;
    if (nlen > (ULONG)HOSTFS_MAX_NAME)
    {
        hl_kill("READDIR name longer than the protocol allows");
        return EDRIVE;
    }

    *next = hl_ld32(hl_rep + 4);

    /*
     * A name this build cannot hold is reported as an EMPTY name with
     * the cookie still advanced, never as the end of the directory: the
     * caller counts what it could not show and carries on, so one
     * unrepresentable name does not hide every file after it.
     */
    if (nlen == 0 || nlen >= namemax)
        return E_OK;

    memcpy(name, hl_rep + HOSTFS_REP_HDR, nlen);
    name[nlen] = '\0';

    return E_OK;
}

LONG hostfs_link_closedir(ULONG handle)
{
    if (hl_batch_handle == handle) hl_batch_len = hl_batch_pos = 0;
    return hl_rpc(HOSTFS_OP_CLOSEDIR, handle, 0, 0, NULL, 0);
}

/*
 * ===========================================================================
 * Attach
 * ===========================================================================
 */

WORD hostfs_link_present(void)
{
    return hl_ready && !hl_dead;
}

static LONG hl_hello(void)
{
    LONG rc = hl_rpc(HOSTFS_OP_HELLO, (ULONG)HOSTFS_PROTO_VERSION, 0, 0,
                     NULL, 0);
    ULONG version, maxdata;

    if (rc < 0)
        return rc;

    if (hl_rep_len < (ULONG)HOSTFS_REP_HDR)
    {
        hl_kill("short HELLO reply");
        return EDRIVE;
    }

    version = hl_ld32(hl_rep + 4);
    maxdata = hl_ld32(hl_rep + 8);

    /*
     * A version we do not understand means fields may have moved, so
     * every later reply would be indexed wrongly.  Refuse the drive
     * rather than read a frame we only half understand.
     */
    if (version != (ULONG)HOSTFS_PROTO_VERSION)
    {
        KINFO(("hostfs-link: helper speaks version %lu, we speak %d\n",
               version, (WORD)HOSTFS_PROTO_VERSION));
        return EDRIVE;
    }

    KINFO(("hostfs-link: slot %d port %d helper v%lu max data %lu\n",
           hl_slot, hl_port, version, maxdata));

    hl_batch_enabled = 0;
    hl_batch_len = hl_batch_pos = 0;
    rc = hl_rpc(HOSTFS_OP_CAPS, 0, 0, 0, NULL, 0);
    if (hl_dead) return EDRIVE;
    if (rc == E_OK)
    {
        if (hl_rep_len != (ULONG)HOSTFS_REP_HDR)
        {
            hl_kill("invalid capability reply");
            return EDRIVE;
        }
        hl_batch_enabled = (hl_ld32(hl_rep + 4) & HOSTFS_CAP_READDIR_BATCH) != 0;
    }
    /* v1 helpers without optional operations reject CAPS; keep READDIR. */

    return E_OK;
}

/*
 * Bring up one virtio-console transport and find our port on it.
 *
 * Called for every console device the probe finds, and gives up on a
 * device rather than on the machine: a second virtio-serial belonging
 * to something else leaves hl_ready clear, and the next device gets its
 * turn.
 */
void hostfs_link_attach(WORD slot)
{
    ULONG max_ports;
    WORD queues_ok[HL_MAX_PORTS];
    WORD i, port, round;

    if (hl_ready)
        return;                 /* the first working link wins */

    /* Nothing survives from a device that did not work out. */
    hl_dead = 0;
    hl_port = -1;
    hl_slot = -1;
    for (i = 0; i < HL_MAX_PORTS; i++)
    {
        hl_port_present[i] = 0;
        hl_port_console[i] = 0;
        hl_port_named[i] = 0;
        hl_port_hostopen[i] = 0;
        queues_ok[i] = 0;
    }

    max_ports = vio_read32(slot, VIRTIO_MMIO_CONFIG + VIRTIO_CONSOLE_CFG_MAX_PORTS);

    /*
     * MULTIPORT has to have been negotiated or the control queues do
     * not exist and the port we were given is not port 0.  The probe
     * asks for it on our behalf; this is the check that it was granted,
     * since a device that declined it looks exactly the same until the
     * first control message vanishes.
     */
    if (max_ports == 0 || max_ports > 0xffffUL)
    {
        KINFO(("hostfs-link: slot %d reports %lu ports; not multiport\n",
               slot, max_ports));
        return;
    }

    if (virtq_setup(&hl_cvq_rx, slot, HL_VQ_CTRL_RX, HL_QSIZE, hl_rings[0]) != 0
     || virtq_setup(&hl_cvq_tx, slot, HL_VQ_CTRL_TX, HL_QSIZE, hl_rings[1]) != 0)
    {
        KINFO(("hostfs-link: slot %d has no control queues\n", slot));
        return;
    }

    /*
     * Every port's queues are brought up BEFORE DRIVER_OK, because
     * which port is ours is only learned from a control message that
     * cannot arrive until after it.  Configuring a queue afterwards is
     * outside what the specification promises, and the cost of not
     * needing to is a few hundred bytes of ring.
     */
    for (i = 0; i < HL_MAX_PORTS; i++)
    {
        if ((ULONG)i >= max_ports)
            break;
        if (virtq_setup(&hl_pvq[i][0], slot, HL_VQ_PORT_RX(i), HL_QSIZE,
                        hl_rings[2 + 2 * i]) == 0
         && virtq_setup(&hl_pvq[i][1], slot, HL_VQ_PORT_TX(i), HL_QSIZE,
                        hl_rings[3 + 2 * i]) == 0)
            queues_ok[i] = 1;
    }

    vio_write32(slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* The device can only speak once it has somewhere to speak into. */
    for (i = 0; i < HL_CTRL_BUFS; i++)
        hl_ctrl_post(i);

    hl_ctrl_send(VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);

    /*
     * Enumerate, then wait for the host end of the chardev.
     *
     * The wait is the point of the loop: QEMU DISCARDS anything written
     * to a port whose far end is not connected, so a HELLO sent before
     * the helper has connected is lost without a trace.  The launcher
     * starts the helper alongside QEMU, so this is a race that really
     * is run, and losing it costs the drive.
     *
     * It is bounded, and expiry is not fatal on its own: HELLO is tried
     * regardless, because a port with no PORT_OPEN is exactly what a
     * device that never sends one looks like, and the reply -- or its
     * absence -- is the honest test.
     */
    port = -1;
    for (round = 0; round < HL_OPEN_ROUNDS; round++)
    {
        hl_ctrl_pump();

        if (port < 0)
        {
            port = hl_pick_port();
            if (port >= 0)
            {
                /* Until this, nothing the host writes reaches us. */
                hl_ctrl_send((ULONG)port, VIRTIO_CONSOLE_PORT_OPEN, 1);
                KDEBUG(("hostfs-link: slot %d using port %d\n", slot, port));
            }
        }

        if (port >= 0 && hl_port_hostopen[port])
            break;
    }

    if (port < 0)
    {
        KINFO(("hostfs-link: slot %d offered no usable port\n", slot));
        return;
    }

    if (!queues_ok[port])
    {
        KINFO(("hostfs-link: slot %d port %d has no data queues\n",
               slot, port));
        return;
    }

    if (!hl_port_hostopen[port])
        KINFO(("hostfs-link: slot %d port %d: host end not connected yet\n",
               slot, port));

    /*
     * The port's queues were registered with the device before
     * DRIVER_OK; this only copies the driver's view of them into the
     * pair the data path uses, so nothing is reconfigured behind the
     * device's back.
     */
    hl_vq_rx = hl_pvq[port][0];
    hl_vq_tx = hl_pvq[port][1];

    hl_slot = slot;
    hl_port = port;

    hl_rx_spin = HL_HELLO_SPIN;
    i = (hl_hello() < 0) ? -1 : 0;
    hl_rx_spin = VIRTQ_SPIN_LIMIT;

    if (i < 0)
    {
        KINFO(("hostfs-link: slot %d port %d: no helper\n", slot, port));
        hl_slot = -1;
        hl_port = -1;
        hl_dead = 0;            /* the NEXT device still deserves a try */
        return;
    }

    hl_ready = 1;
}

#endif /* MACHINE_QEMU_VIRT */
