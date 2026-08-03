/*
 * hostfs_name.c - 8.3 name mapping for the host-folder drive
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Pure name logic: no I/O, no 9P, no GEMDOS. Kept separate so the
 * rules can be tested natively.
 *
 * The host->Atari and Atari->host conversions are NOT inverses, and
 * that asymmetry is deliberate rather than an oversight -- it is what
 * the reference implementation does. This file is the host->Atari
 * (display) direction: extension is the text after the LAST dot.
 */

#include "hostfs_name.h"

#define INVALID_CHAR '+'

/* Bytes below 32, byte 127, and these become INVALID_CHAR. */
static int is_invalid(unsigned char c)
{
    if (c < 32 || c == 127)
        return 1;
    return c == '*' || c == ':' || c == '?' || c == '\\' || c == '/';
}

static char convert(unsigned char c)
{
    if (is_invalid(c))
        return INVALID_CHAR;
    if (c >= 128)
        return (char)c;             /* raw high bytes pass through */
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return (char)c;
}

void hostfs_host_to_atari(const char *host, char *out)
{
    const char *dot = 0;
    const char *p;
    int base = 0, ext = 0, n = 0;

    /* ".." is the sole name exempt from the dot rule. */
    if (host[0] == '.' && host[1] == '.' && host[2] == '\0')
    {
        out[0] = '.'; out[1] = '.'; out[2] = '\0';
        return;
    }

    for (p = host; *p; p++)
        if (*p == '.')
            dot = p;                /* strrchr: the LAST dot */

    for (p = host; *p; p++)
    {
        if (p == dot)
            break;
        if (base >= 8)
            continue;
        /* A dot before the last one is not a separator. */
        out[n++] = (*p == '.') ? INVALID_CHAR : convert((unsigned char)*p);
        base++;
    }

    if (dot)
    {
        out[n++] = '.';
        for (p = dot + 1; *p && ext < 3; p++, ext++)
            out[n++] = convert((unsigned char)*p);
    }

    out[n] = '\0';
}
