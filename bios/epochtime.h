/*
 * epochtime.h - Unix epoch seconds to GEMDOS packed date/time
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#ifndef EPOCHTIME_H
#define EPOCHTIME_H

#include "portab.h"

/*
 * Convert Unix epoch seconds (UTC) to the GEMDOS packed date/time
 * fields used by DTA::d_date/d_time and Tgetdate()/Tgettime():
 *
 *   date = (year - 1980) << 9 | month << 5 | day     (month 1-12, day 1-31)
 *   time = hour << 11 | minute << 5 | second / 2      (2-second resolution)
 *
 * Timestamps before 1980-01-01 clamp only the year field to 0; month
 * and day are left as the real month/day of that date. This matches
 * the reference implementation (Hatari's gemdos.c) rather than folding
 * everything before 1980 to 1980-01-01.
 *
 * Pure arithmetic, no I/O -- safe to call from any context.
 */
void epoch_to_dos(ULONG secs, UWORD *dos_date, UWORD *dos_time);

#endif /* EPOCHTIME_H */
