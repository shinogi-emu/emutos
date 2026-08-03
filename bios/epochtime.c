/*
 * epochtime.c - Unix epoch seconds to GEMDOS packed date/time
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Pure arithmetic, no I/O: kept separate from both hostfs.c and
 * qemuvirt.c so it can be exercised with a native host test the way
 * hostfs_name.c is.
 *
 * -mshort makes "int" 16 bits on this target, and year/day/second
 * counts here all exceed 32767, so every intermediate is carried in
 * ULONG (32-bit unsigned), never plain int/UWORD.
 */

#include "config.h" /* CONF_* / MACHINE_* defines -- required for the
                      * #ifdef MACHINE_QEMU_VIRT guard below to be real.
                      * A file that omits this and still guards itself
                      * silently compiles to an empty translation unit
                      * on every target. */
#include "portab.h"
#include "epochtime.h"

#ifdef MACHINE_QEMU_VIRT

/*
 * Days since the Unix epoch (1970-01-01) -> proleptic Gregorian
 * calendar date. This is Howard Hinnant's civil_from_days algorithm
 * (public domain, http://howardhinnant.github.io/date_algorithms.html),
 * specialised to days >= 0: the caller only ever has dates from 1970
 * onward (epoch_to_dos() takes an unsigned second count), so the
 * negative-era branch of the general algorithm is not needed.
 *
 * All intermediates fit comfortably in 32 bits for every date this
 * machine can represent (up to the ULONG-seconds rollover in 2106),
 * so plain ULONG arithmetic is exact -- no wraparound, no precision
 * loss.
 */
static void days_to_civil(ULONG z, ULONG *y_out, ULONG *m_out, ULONG *d_out)
{
    ULONG era, doe, yoe, y, doy, mp, m, d;

    z += 719468UL;                     /* shift to era starting 0000-03-01 */
    era = z / 146097UL;                /* 146097 days per 400-year era */
    doe = z - era * 146097UL;          /* day of era,  [0, 146096] */
    yoe = (doe - doe / 1460UL + doe / 36524UL - doe / 146096UL) / 365UL;
                                        /* year of era, [0, 399] */
    y = yoe + era * 400UL;
    doy = doe - (365UL * yoe + yoe / 4UL - yoe / 100UL); /* day of year, [0, 365] */
    mp = (5UL * doy + 2UL) / 153UL;    /* month, shifted so March = 0, [0, 11] */
    d = doy - (153UL * mp + 2UL) / 5UL + 1UL; /* day of month, [1, 31] */

    if (mp < 10UL)
        m = mp + 3UL;                  /* Mar..Dec -> 3..12 */
    else
        m = mp - 9UL;                  /* Jan..Feb -> 1..2 */

    if (m <= 2UL)
        y += 1UL;                      /* Jan/Feb belong to the next real year */

    *y_out = y;
    *m_out = m;
    *d_out = d;
}

void epoch_to_dos(ULONG secs, UWORD *dos_date, UWORD *dos_time)
{
    ULONG days, rem;
    ULONG y, m, d;
    ULONG hour, minute, second;
    LONG year_field;

    days = secs / 86400UL;
    rem = secs % 86400UL;

    hour = rem / 3600UL;
    minute = (rem % 3600UL) / 60UL;
    second = rem % 60UL;

    days_to_civil(days, &y, &m, &d);

    /*
     * Years before 1980 cannot be represented in the 7-bit year field.
     * Clamp the year to 0 (=> 1980) but leave month/day as the real
     * values for that date -- NOT folded to 1980-01-01. This matches
     * the reference implementation (Hatari gemdos.c) rather than
     * inventing a friendlier-looking but wrong fallback date.
     */
    year_field = (LONG)y - 1980L;
    if (year_field < 0L)
        year_field = 0L;

    *dos_date = (UWORD)(((ULONG)year_field << 9) | (m << 5) | d);
    *dos_time = (UWORD)((hour << 11) | (minute << 5) | (second / 2UL));
}

#endif /* MACHINE_QEMU_VIRT */
