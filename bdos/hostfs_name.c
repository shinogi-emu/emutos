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

#include "config.h"
#include "hostfs_name.h"

#ifdef MACHINE_QEMU_VIRT

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

/*
 * The lookup direction. Note the differences from the display
 * direction above: the extension starts at the FIRST dot, characters
 * are neither substituted nor upcased, and nothing is exempt.
 */
void hostfs_atari_clip(const char *name, char *out)
{
    const char *p;
    int base = 0, ext = 0, n = 0;

    for (p = name; *p && *p != '.'; p++)    /* strchr: the FIRST dot */
    {
        if (base < 8)
        {
            out[n++] = *p;
            base++;
        }
    }

    if (*p == '.')
    {
        out[n++] = '.';
        for (p++; *p && *p != '.' && ext < 3; p++, ext++)
            out[n++] = *p;
    }

    out[n] = '\0';
}

/*
 * Plain byte order, deliberately case sensitive.
 *
 * The reference implementation sorts with alphasort, which is strcoll,
 * but never calls setlocale -- so the locale stays "C" and strcoll
 * degenerates to strcmp. All A-Z therefore sort before all a-z. Its
 * manual documents this as case-insensitive and is wrong; matching the
 * manual here would reorder AUTO folder execution.
 */
int hostfs_name_compare(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (*x && *x == *y)
    {
        x++;
        y++;
    }

    return (int)*x - (int)*y;
}

#endif /* MACHINE_QEMU_VIRT */
