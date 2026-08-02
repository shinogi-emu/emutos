/*
 * qemuvirt2.S - QEMU m68k "virt" machine, assembler support
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "asmdefs.h"

#ifdef MACHINE_QEMU_VIRT

        .globl  _qemuvirt_timer_int

        .extern _qemuvirt_timer_ack
        .extern _int_vbl
        .extern _vector_5ms

        .text

/*
 * Level 6 interrupt handler.
 *
 * Both goldfish RTCs are wired to PIC #6, so this one vector covers the
 * 200 Hz system tick and the VBL, and both can be pending at once --
 * which happens on every fourth tick, since 200 Hz and 50 Hz coincide.
 * _qemuvirt_timer_ack() reads the PIC, silences whatever fired, re-arms
 * it, and returns a bitmask saying which it was.
 *
 * The mask is held in d2 across the call to _int_vbl: d2 is callee-saved
 * in the m68k C ABI, so it survives, and the four scratch registers are
 * all we need to preserve for the interrupted code.
 *
 * int_vbl() is called as an ordinary subroutine.  It is rts-terminated
 * whenever CONF_WITH_ATARI_VIDEO is 0 (see the end of _int_vbl in
 * vectors.S), which is the same way MACHINE_LISA reaches it.
 */
_qemuvirt_timer_int:
        movem.l d0-d2/a0-a1,-(sp)

        jbsr    _qemuvirt_timer_ack     // d0 = which sources fired
        move.w  d0,d2                   // survives the jbsr below

        btst    #1,d2                   // QEMUVIRT_FIRED_VBL
        jeq     no_vbl
        jbsr    _int_vbl
no_vbl:
        btst    #0,d2                   // QEMUVIRT_FIRED_TICK
        jeq     no_tick

        movem.l (sp)+,d0-d2/a0-a1

        /*
         * Tail into the 200 Hz handler through the vector_5ms
         * indirection rather than jumping at _int_timerc directly: that
         * pointer is what the _5MS cookie publishes, so FreeMiNT can
         * hook the tick.
         *
         * Pushing the target and returning to it avoids clobbering a
         * register we have just restored.  _int_timerc ends in rte, not
         * rts, and it deliberately leaves the exception frame for that
         * -- which is exactly what is left on the stack here.
         */
        move.l  _vector_5ms,-(sp)
        rts

no_tick:
        movem.l (sp)+,d0-d2/a0-a1
        rte

#endif /* MACHINE_QEMU_VIRT */
