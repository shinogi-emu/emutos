/*
 * shinogi_splash.c - draw the logo into the framebuffer at boot
 *
 * The guest spends a long blank stretch bringing up hostfs before anything
 * is drawn.  This fills it, and it does so inside the emulated screen, so
 * one implementation serves Windows, Linux and macOS and it ships as an
 * EMUTOS.ELF drop-in rather than as a new installer.
 *
 * No GPU code is needed.  virtio_gpu_present() already reads v_bas_ad as
 * RGB565 and pushes it off the VBL timer, whoever wrote it, so writing
 * pixels is the whole job.
 *
 * Drawn once and never touched again: boot text scrolls over it from the
 * top left, then fVDI, then the AES paint over it in turn.  Suppressing
 * the boot text would give a cleaner logo and would cost on-screen
 * diagnosis of early boot failures, which is exactly when it is wanted.
 *
 * The image is 2 bits of colour per pixel, run-length encoded, and is
 * generated from assets/shinogi_logo.svg by tools/make-icons.py in the
 * shinogi repository.  Do not edit shinogi_splash.h by hand.
 */

#include "emutos.h"
#include "screen.h"
#include "tosvars.h"        /* v_bas_ad */
#include "lineavars.h"      /* v_planes, V_REZ_HZ, V_REZ_VT, BYTES_LIN */
#include "shinogi_splash.h"

#if CONF_WITH_SHINOGI_SPLASH

void shinogi_splash(void)
{
    UWORD *fb = (UWORD *)v_bas_ad;
    ULONG stride;               /* pixels per scanline */
    WORD ox, oy;                /* top left of the logo */
    ULONG i = 0;
    WORD x, y;

    if (!fb)
        return;

    /*
     * 16-bit only.  Everything else this machine can be asked for is
     * either a planar mode fVDI will replace or a depth this does not
     * know how to write, and a splash is never worth risking the boot.
     */
    if (v_planes != 16)
        return;

    stride = (ULONG)BYTES_LIN / sizeof(UWORD);

    /* Centre it, and simply decline if the screen is too small rather
     * than clipping -- every mode the launcher offers is far larger. */
    if (V_REZ_HZ < SPLASH_W || V_REZ_VT < SPLASH_H)
        return;
    ox = (V_REZ_HZ - SPLASH_W) / 2;
    oy = (V_REZ_VT - SPLASH_H) / 2;

    for (y = 0; y < SPLASH_H; y++)
    {
        UWORD *row = fb + (ULONG)(oy + y) * stride + ox;

        x = 0;
        while (x < SPLASH_W)
        {
            UBYTE count, index;

            if (i + 1 >= SPLASH_RLE_LEN)
                return;                 /* truncated: stop, never run on */
            count = splash_rle[i++];
            index = splash_rle[i++];

            if (count > SPLASH_W - x)   /* corrupt run, do not overrun */
                count = SPLASH_W - x;

            if (index)                  /* 0 is transparent */
            {
                UWORD colour = splash_pal[index];
                WORD n;

                for (n = 0; n < count; n++)
                    row[x + n] = colour;
            }
            x += count;
        }
    }
}

#endif /* CONF_WITH_SHINOGI_SPLASH */
