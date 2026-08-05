/*
 * virtio_input.c - virtio-input keyboard and tablet
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "virtio.h"
#include "virtio_input.h"
#include "qemuvirt.h"
#include "ikbd.h"
#include "lineavars.h"
#include "biosdefs.h"
#include "kprint.h"

#ifdef MACHINE_QEMU_VIRT

/*
 * Linux evdev keycode to Atari ST scancode.
 *
 * The first 68 codes are identical: evdev numbering was taken from AT
 * scancode set 1, and the Atari ST keyboard follows the same layout for
 * the main block. So ESC through F10 needs no translation at all and
 * the table is only interesting where the two keyboards genuinely
 * differ -- the keypad, the arrow cluster and the ISO key.
 *
 * 0 means unmapped; those keys are dropped.
 */
static const UBYTE atari_from_evdev[128] =
{
    /* 0x00 */ 0,
    /* 0x01-0x36: ESC, digits, letters, punctuation -- identical */
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    /*
     * 0x37 KEY_KPASTERISK. NOT the identity: Atari 0x37 is claimed by
     * the extended-mouse handler for button 3, so it would be
     * swallowed before reaching the keyboard. The keypad '*' is 0x66.
     */
    0x66,
    /* 0x38-0x44: LALT, SPACE, CAPS, F1-F10 -- identical */
    0x38, 0x39, 0x3a,
    0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44,
    /* 0x45 NUMLOCK, 0x46 SCROLLLOCK: no Atari equivalent */
    0, 0,
    /* 0x47-0x53: keypad. Atari orders these differently from AT. */
    0x67,   /* KP7 */
    0x68,   /* KP8 */
    0x69,   /* KP9 */
    0x4a,   /* KPMINUS */
    0x6a,   /* KP4 */
    0x6b,   /* KP5 */
    0x6c,   /* KP6 */
    0x4e,   /* KPPLUS */
    0x6d,   /* KP1 */
    0x6e,   /* KP2 */
    0x6f,   /* KP3 */
    0x70,   /* KP0 */
    0x71,   /* KPDOT */
    /* 0x54-0x55 */
    0, 0,
    /* 0x56 KEY_102ND -- the extra ISO key */
    0x60,
    /* 0x57 F11, 0x58 F12: the Atari has none. Shift+F1..F10 gives
     * F11..F20, and EmuTOS synthesises that itself. */
    0, 0,
    /* 0x59-0x5f */
    0, 0, 0, 0, 0, 0, 0,
    /* 0x60 KPENTER */
    0x72,
    /* 0x61 RIGHTCTRL -- the Atari has a single Control */
    0x1d,
    /* 0x62 KPSLASH */
    0x65,
    /* 0x63 SYSRQ */
    0,
    /* 0x64 RIGHTALT -- single Alternate */
    0x38,
    /* 0x65 */
    0,
    /* 0x66 HOME */
    0x47,
    /* 0x67 UP */
    0x48,
    /* 0x68 PAGEUP */
    0,
    /* 0x69 LEFT */
    0x4b,
    /* 0x6a RIGHT */
    0x4d,
    /* 0x6b END */
    0,
    /* 0x6c DOWN */
    0x50,
    /* 0x6d PAGEDOWN */
    0,
    /* 0x6e INSERT */
    0x52,
    /* 0x6f DELETE */
    0x53,
    /* 0x70-0x7f */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/*
 * The mouse position line-A variables. They live in bios/lineavars.S
 * but are only declared in vdi/vdi_defs.h, which is not on the BIOS
 * include path; declaring them here avoids either adding a VDI include
 * path to a BIOS file or duplicating the declaration into
 * include/lineavars.h, which would clash under -Werror=redundant-decls.
 */
extern WORD GCURX;
extern WORD GCURY;

/* One device: its queue, its ring and its event buffers. */
typedef struct
{
    WORD slot;
    WORD active;
    VIRTQ vq;
    UBYTE *ring;
    UBYTE *buf;
} VIDEV;

static VIDEV kbd_dev;
static VIDEV tab_dev;

static UBYTE kbd_ring[2048] __attribute__((aligned(16)));
static UBYTE kbd_buf[VI_QSIZE * VI_EVENT_SIZE] __attribute__((aligned(4)));
static UBYTE tab_ring[2048] __attribute__((aligned(16)));
static UBYTE tab_buf[VI_QSIZE * VI_EVENT_SIZE] __attribute__((aligned(4)));

/* Pointer state, accumulated across a report and applied on SYN. */
static WORD abs_x, abs_y;       /* 0..VI_ABS_MAX */
static WORD have_pos;
static WORD btn_state;          /* bit0 left, bit1 right */
static WORD pending_move;
static WORD pending_btn;

static UWORD ld16(const UBYTE *p)
{
    return (UWORD)(p[0] | ((UWORD)p[1] << 8));
}

static void st16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v & 0xff);
    p[1] = (UBYTE)(v >> 8);
}

static ULONG ld32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/*
 * Config space probe.
 *
 * select and subsel must be written as a pair with no read in between:
 * every read regenerates the whole config buffer and zeroes those two
 * bytes when the selection does not match, so a read between the writes
 * silently latches select back to 0.
 *
 * Returns the payload size, 0 if the device does not support it.
 */
static WORD vi_cfg(WORD slot, UBYTE select, UBYTE subsel, UBYTE *out, WORD max)
{
    volatile UBYTE *cfg =
        (volatile UBYTE *)(VIRTIO_SLOT(slot) + VIRTIO_MMIO_CONFIG);
    WORD size, i;

    cfg[VI_CFG_SELECT] = select;
    cfg[VI_CFG_SUBSEL] = subsel;

    size = cfg[VI_CFG_SIZE];
    if (size > max)
        size = max;

    for (i = 0; i < size; i++)
        out[i] = cfg[VI_CFG_DATA + i];

    return size;
}

/* Bit n of an evdev bitmap, LSB first within each byte. */
static WORD bit_set(const UBYTE *map, WORD size, WORD n)
{
    if ((n >> 3) >= size)
        return 0;
    return (map[n >> 3] >> (n & 7)) & 1;
}

/*
 * Post every descriptor as device-writable and hand them to the device.
 *
 * A descriptor without VRING_DESC_F_WRITE yields no writable space, so
 * the event is copied nowhere and returned with a length of zero -- a
 * silent loss rather than an error.
 */
static void vi_post_all(VIDEV *d)
{
    WORD i;

    for (i = 0; i < VI_QSIZE; i++)
    {
        virtq_add(&d->vq, i, (ULONG)(d->buf + i * VI_EVENT_SIZE),
                  VI_EVENT_SIZE, VRING_DESC_F_WRITE);
        st16(d->vq.avail + 4 + 2 * i, (UWORD)i);
    }

    /*
     * Suppress the completion interrupt outright. We drain from the
     * timer, and with EVENT_IDX not negotiated this flag is all it
     * takes for the device never to raise the line at all -- so there
     * is no ISR to acknowledge and no spurious interrupt to chase.
     */
    st16(d->vq.avail, VRING_AVAIL_F_NO_INTERRUPT);

    d->vq.avail_idx = VI_QSIZE;
    st16(d->vq.avail + 2, d->vq.avail_idx);
}

/* Give one consumed descriptor back to the device. */
static void vi_recycle(VIDEV *d, WORD head)
{
    st16(d->vq.avail + 4 + 2 * (d->vq.avail_idx % d->vq.num), (UWORD)head);
    d->vq.avail_idx++;
    st16(d->vq.avail + 2, d->vq.avail_idx);
}

/*
 * Record a device found by the probe.
 *
 * Classification is by capability rather than by name: a device that
 * reports ABS_X is a tablet, one that reports the Escape key is a
 * keyboard. Names would work too but are a QEMU string rather than a
 * property of the device.
 *
 * All of this happens before DRIVER_OK, deliberately: a config write
 * after that point raises a config-change interrupt.
 */
void virtio_input_attach(WORD slot)
{
    UBYTE bits[64];
    WORD size;

    size = vi_cfg(slot, VI_CFG_EV_BITS, EV_ABS, bits, sizeof(bits));
    if (size > 0 && bit_set(bits, size, ABS_X))
    {
        if (!tab_dev.active)
        {
            tab_dev.slot = slot;
            tab_dev.active = 1;
            tab_dev.ring = tab_ring;
            tab_dev.buf = tab_buf;
            KINFO(("input: slot %d is a tablet\n", slot));
        }
        return;
    }

    size = vi_cfg(slot, VI_CFG_EV_BITS, EV_KEY, bits, sizeof(bits));
    if (size > 0 && bit_set(bits, size, 1 /* KEY_ESC */))
    {
        if (!kbd_dev.active)
        {
            kbd_dev.slot = slot;
            kbd_dev.active = 1;
            kbd_dev.ring = kbd_ring;
            kbd_dev.buf = kbd_buf;
            KINFO(("input: slot %d is a keyboard\n", slot));
        }
        return;
    }

    KINFO(("input: slot %d not recognised\n", slot));
}

static void vi_start(VIDEV *d)
{
    if (!d->active)
        return;

    if (virtq_setup(&d->vq, d->slot, 0, VI_QSIZE, d->ring) != 0)
    {
        d->active = 0;
        return;
    }

    vi_post_all(d);

    /* Events only begin to flow once DRIVER_OK is set. */
    vio_write32(d->slot, VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

/*
 * Called from kbd_init(), beside the Amiga and Lisa hooks.
 *
 * By this point init_acia_vecs() has installed kbdvec and the IOREC,
 * and bioskeys() has selected a key table -- both are required before
 * a single scancode may be delivered.
 */
void virtio_input_init(void)
{
    vi_start(&kbd_dev);
    vi_start(&tab_dev);
}

/*
 * Move the pointer to an absolute position.
 *
 * The IKBD protocol only carries relative deltas, and the VDI drops the
 * absolute report outright -- mouse_int tests for a 0xF8..0xFB header
 * and an absolute packet fails that test. So the target is turned into
 * deltas here.
 *
 * That conversion is exact rather than approximate: mouse_int does a
 * plain signed add with no acceleration and no scaling, so aiming at
 * GCURX/GCURY lands precisely. The host and guest pointers cannot drift
 * apart the way they would with a relative mouse.
 *
 * The target is clamped first. scrn_clip applies after the add, so a
 * target outside the screen would leave GCURX saturated with the delta
 * still non-zero, and the loop would never converge.
 */
static void vi_move(WORD tx, WORD ty)
{
    UBYTE packet[3];
    WORD guard;

    if (tx < 0)
        tx = 0;
    if (ty < 0)
        ty = 0;
    if (tx > DEV_TAB[0])
        tx = DEV_TAB[0];
    if (ty > DEV_TAB[1])
        ty = DEV_TAB[1];

    for (guard = 0; guard < 16; guard++)
    {
        WORD dx = tx - GCURX;
        WORD dy = ty - GCURY;
        WORD sx, sy;

        if (dx == 0 && dy == 0 && !pending_btn)
            return;

        sx = (dx > 127) ? 127 : (dx < -127) ? -127 : dx;
        sy = (dy > 127) ? 127 : (dy < -127) ? -127 : dy;

        packet[0] = 0xf8;
        if (btn_state & 1)
            packet[0] |= 0x02;      /* left  */
        if (btn_state & 2)
            packet[0] |= 0x01;      /* right */
        packet[1] = (UBYTE)sx;
        packet[2] = (UBYTE)sy;

        KDEBUG(("input: move to %d,%d (dx %d dy %d btn %d)\n",
                tx, ty, sx, sy, btn_state));
        call_mousevec(packet);
        pending_btn = 0;
    }
}

/* Apply everything accumulated since the last SYN. */
static void vi_sync(void)
{
    KDEBUG(("input: sync move=%d btn=%d pos=%d,%d have=%d devtab=%d,%d\n",
            pending_move, pending_btn, abs_x, abs_y, have_pos,
            DEV_TAB[0], DEV_TAB[1]));

    if (!pending_move && !pending_btn)
        return;

    if (have_pos)
    {
        /*
         * Shift rather than divide by (VI_ABS_MAX + 1). EmuTOS builds
         * with -mshort, so that expression is evaluated in a 16-bit int
         * and 32768 overflows to -32768 -- every target then clamps to
         * zero and the pointer sticks in the top-left corner.
         */
        LONG tx = ((LONG)abs_x * (DEV_TAB[0] + 1)) >> 15;
        LONG ty = ((LONG)abs_y * (DEV_TAB[1] + 1)) >> 15;

        vi_move((WORD)tx, (WORD)ty);
    }

    pending_move = 0;
}

static void vi_key(UWORD code, ULONG value)
{
    UBYTE sc;

    if (code >= 128)
        return;

    sc = atari_from_evdev[code];
    if (sc == 0)
        return;

    /*
     * value is only ever 0 or 1 here -- QEMU never sends the evdev
     * autorepeat value of 2, and host repeat arrives as repeated
     * presses with no intervening release. EmuTOS runs its own repeat
     * off the 200 Hz tick, so those extra presses are simply passed on.
     */
    if (!value)
        sc |= 0x80;

    KDEBUG(("input: key %d -> scancode 0x%02x\n", code, sc));
    call_ikbdraw(sc);
}

/* Drain one device's used ring. */
static void vi_drain(VIDEV *d)
{
    UWORD used_idx;

    if (!d->active)
        return;

    used_idx = ld16(d->vq.used + 2);

    while (d->vq.last_used != used_idx)
    {
        UBYTE *elem = d->vq.used + 4
                    + VIRTQ_USED_ELEM_SIZE * (ULONG)(d->vq.last_used % d->vq.num);
        WORD head = (WORD)ld32(elem);
        const UBYTE *ev;
        UWORD type, code;
        ULONG value;

        if (head < 0 || head >= VI_QSIZE)
            break;

        ev = d->buf + head * VI_EVENT_SIZE;
        type = ld16(ev);
        code = ld16(ev + 2);
        value = ld32(ev + 4);

        KDEBUG(("input: slot %d ev type=%d code=0x%x val=%ld\n",
                d->slot, type, code, value));

        switch (type)
        {
        case EV_KEY:
            if (code == BTN_LEFT)
            {
                btn_state = value ? (btn_state | 1) : (btn_state & ~1);
                pending_btn = 1;
            }
            else if (code == BTN_RIGHT)
            {
                btn_state = value ? (btn_state | 2) : (btn_state & ~2);
                pending_btn = 1;
            }
            else if (code == BTN_MIDDLE)
            {
                /*
                 * Eiffel scancode for mouse button 3. The extra buttons
                 * and the wheel reach GEM as IKBD scancodes rather than
                 * through the mouse packet, so they go out the same way
                 * a key does. Only the press is sent -- 0x37 is a
                 * make code with no matching break in the protocol.
                 */
                if (value)
                    call_ikbdraw(0x37);
            }
            else
            {
                vi_key(code, value);
            }
            break;

        case EV_REL:
            /*
             * Wheel. QEMU reports one notch per event, positive away
             * from the user. EmuTOS turns 0x59/0x5a back into a wheel
             * event for the AES (bios/ikbd.c), which is why this is a
             * scancode and not a mouse packet.
             */
            if (code == REL_WHEEL)
            {
                /* value is signed on the wire; the field is unsigned. */
                LONG v = (LONG)value;
                LONG n = (v < 0) ? -v : v;

                while (n-- > 0)
                    call_ikbdraw((v > 0) ? 0x59 : 0x5a);
            }
            break;

        case EV_ABS:
            if (code == ABS_X)
            {
                abs_x = (WORD)value;
                have_pos = 1;
                pending_move = 1;
            }
            else if (code == ABS_Y)
            {
                abs_y = (WORD)value;
                have_pos = 1;
                pending_move = 1;
            }
            break;

        case EV_SYN:
            vi_sync();
            break;
        }

        d->vq.last_used++;
        vi_recycle(d, head);
    }
}

/*
 * Drain both devices.
 *
 * Called from the timer rather than from an interrupt: the device
 * raises one interrupt per report at most, and with the completion
 * interrupt suppressed there is none at all. Polling costs two 16-bit
 * reads when nothing has happened.
 */
void virtio_input_poll(void)
{
    vi_drain(&kbd_dev);
    vi_drain(&tab_dev);
}

#endif /* MACHINE_QEMU_VIRT */
