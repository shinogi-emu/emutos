/* Changed framebuffer rows for virtio-gpu, GPL-2.0-or-later. */
#ifndef GPU_DAMAGE_H
#define GPU_DAMAGE_H

typedef struct {
    UWORD x, y, w, h;
} GPU_DAMAGE;

/* Widths are even. Compare two pixels at a time, without colour lookups. */
static UWORD gpu_first_changed(const UWORD *pixels, const UWORD *previous,
                               UWORD width)
{
#if defined(__GNUC__) && defined(__mc68000__) && !defined(__mcoldfire__)
    const ULONG *a = (const ULONG *)pixels;
    const ULONG *b = (const ULONG *)previous;
    UWORD remaining = width / 8 - 1;
    UWORD changed;

    __asm__ volatile (
        "1: movem.l (%1)+,%%d0-%%d3\n\t"
        "cmp.l (%2)+,%%d0\n\tbne 2f\n\t"
        "cmp.l (%2)+,%%d1\n\tbne 2f\n\t"
        "cmp.l (%2)+,%%d2\n\tbne 2f\n\t"
        "cmp.l (%2)+,%%d3\n\tbne 2f\n\t"
        "dbra %0,1b\n\t"
        "moveq #0,%%d0\n\tbra 3f\n"
        "2: moveq #1,%%d0\n"
        "3: move.w %%d0,%3"
        : "+d" (remaining), "+a" (a), "+a" (b), "=d" (changed)
        : : "d0", "d1", "d2", "d3", "cc", "memory");
    return changed ? (UWORD)(((ULONG)b - (ULONG)previous) / 2 - 2) : width;
#else
    UWORD x;
    for (x = 0; x < width; x += 2)
        if (pixels[x] != previous[x] || pixels[x + 1] != previous[x + 1])
            return x;
    return width;
#endif
}

/* Convert only changed spans. The result encloses every changed pixel. */
static WORD gpu_damage_update(const UWORD *pixels, UWORD *previous,
                              ULONG *argb, const ULONG *colours,
                              UWORD width, UWORD height, WORD force,
                              GPU_DAMAGE *damage)
{
    UWORD y, left = width, right = 0, top = height, bottom = 0;

    if (!width || !height || (width & 7)) return 0;
    for (y = 0; y < height; y++)
    {
        UWORD first = force ? 0 : gpu_first_changed(pixels, previous, width);
        UWORD last = width;
        UWORD x;
        if (first != width)
        {
            if (!force)
                while (last > first + 2 && pixels[last - 1] == previous[last - 1]
                       && pixels[last - 2] == previous[last - 2])
                    last -= 2;
            for (x = first; x < last; x++)
            {
                UWORD pixel = pixels[x];
                previous[x] = pixel;
                argb[x] = colours[pixel];
            }
            if (first < left) left = first;
            if (last > right) right = last;
            if (y < top) top = y;
            bottom = y + 1;
        }
        pixels += width;
        previous += width;
        argb += width;
    }
    if (!right) return 0;
    damage->x = left;
    damage->y = top;
    damage->w = right - left;
    damage->h = bottom - top;
    return 1;
}

#endif
