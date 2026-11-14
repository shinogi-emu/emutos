/*
 * vdi_ft2.c - the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * This file is compiled with 32-bit int, like FreeType (see the Makefile).
 * It only meets the rest of the VDI through vdi_ft2.h, and uses the file
 * and memory services of vdi_ftos.h.
 */

#include "vdi_ft2.h"
#include "vdi_ftos.h"

#include <ft2build.h>
#include FT_FREETYPE_H


static FT_Library library;


/*
 * start FreeType
 *
 * returns 0 if all is well, or a negative number
 */
LONG vdi_ft_init(void)
{
    FT_Error error;

    if (library)
        return 0;

    error = FT_Init_FreeType(&library);
    if (error)
    {
        library = NULL;
        return -(LONG)error;
    }

    return 0;
}


void vdi_ft_term(void)
{
    if (library)
    {
        FT_Done_FreeType(library);
        library = NULL;
    }
}
