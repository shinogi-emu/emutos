/*
 * virtio_input.h - virtio-input keyboard and tablet
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#ifdef MACHINE_QEMU_VIRT

/* Event types -- include/standard-headers/linux/input-event-codes.h */
#define EV_SYN      0x00
#define EV_KEY      0x01
#define EV_REL      0x02
#define EV_ABS      0x03

#define SYN_REPORT  0x00

#define ABS_X       0x00
#define ABS_Y       0x01
#define REL_WHEEL   0x08

#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/*
 * Config space at MMIO 0x100. Little-endian, and byte access needs no
 * swap, so everything here is read a byte at a time.
 *
 *   0x00 select   (write)
 *   0x01 subsel   (write)
 *   0x02 size     (read: valid bytes of payload, 0 = unsupported)
 *   0x08 payload
 */
#define VI_CFG_SELECT   0x00
#define VI_CFG_SUBSEL   0x01
#define VI_CFG_SIZE     0x02
#define VI_CFG_DATA     0x08

#define VI_CFG_UNSET        0x00
#define VI_CFG_ID_NAME      0x01
#define VI_CFG_ID_DEVIDS    0x03
#define VI_CFG_EV_BITS      0x11
#define VI_CFG_ABS_INFO     0x12

/* Tablet coordinates are always 0..32767 regardless of screen size. */
#define VI_ABS_MAX      32767

/* One report is at most a few events; 64 buffers is ~20 reports of slack. */
#define VI_QSIZE        64
#define VI_EVENT_SIZE   8

void virtio_input_attach(WORD slot);
void virtio_input_init(void);
void virtio_input_poll(void);

#endif /* MACHINE_QEMU_VIRT */

#endif /* VIRTIO_INPUT_H */
