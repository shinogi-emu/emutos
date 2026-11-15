FreeType in EmuTOS
==================

This directory holds the part of FreeType that the VDI uses to draw outline
(scalable) fonts.  It is only built when CONF_WITH_VDI_OUTLINE_FONTS is 1
(see include/config.h).

Version:    FreeType 2.13.3
Tarball:    freetype-2.13.3.tar.xz
SHA256:     0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
Source:     https://sourceforge.net/projects/freetype/files/freetype2/2.13.3/

Licence
-------

FreeType is used here under the terms of the GNU General Public License,
version 2 (GPLv2.TXT), one of the two licences FreeType is offered under.
The other one is the FreeType License (FTL.TXT).  LICENSE.TXT is the
FreeType file that explains this choice.

Contents
--------

include/    the public and internal headers of the tarball, unchanged.
src/        the sources of the modules that are built, unchanged:
              base      the library core
              sfnt      the SFNT container (TrueType and OpenType files)
              truetype  the TrueType driver, with its bytecode interpreter
              cff       the CFF (OpenType with PostScript outlines) driver
              psaux     PostScript support used by cff
              pshinter  the PostScript hinter used by cff
              psnames   PostScript glyph names
              smooth    the anti-aliased rasterizer
              raster    the monochrome rasterizer (FT_RENDER_MODE_MONO)
              autofit   the automatic hinter
            Not included: the bdf, pcf, pfr, type1, type42, winfnt, lzw,
            gzip and cache modules.  This
            means there is no zlib: compressed fonts are not supported.
config/     the configuration used by the EmuTOS build; the Makefile puts
            this directory in front of include/ and selects the files with
            FT_CONFIG_OPTIONS_H, FT_CONFIG_MODULES_H and
            FT_CONFIG_STANDARD_LIBRARY_H:
              ftoption.h       include/freetype/config/ftoption.h with these
                               options turned off: FT_CONFIG_OPTION_USE_LZW,
                               FT_CONFIG_OPTION_USE_ZLIB,
                               FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES,
                               FT_CONFIG_OPTION_MAC_FONTS,
                               FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK,
                               FT_CONFIG_OPTION_INCREMENTAL,
                               FT_CONFIG_OPTION_SVG,
                               TT_CONFIG_OPTION_COLOR_LAYERS,
                               TT_CONFIG_OPTION_GX_VAR_SUPPORT,
                               TT_CONFIG_OPTION_BDF
                             and this one turned on:
                               FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT
                             (fonts are always loaded into memory first),
                             and FT_RENDER_POOL_SIZE halved to 8192: the
                             rasterizers keep that pool on the stack, and
                             draw the same pixels with the smaller one.
              ftmodule.h       the modules listed above: autofit, truetype,
                               cff, psaux, psnames, pshinter, sfnt, smooth,
                               raster.
              vdi_ftstdlib.h   replaces ftstdlib.h: the C library is the
                               one in vdi/vdi_ftlibc.c.  It does not define
                               FT_LLONG_MAX and friends on purpose, so that
                               FreeType does not use 64-bit integers.

The files of the tarball are not modified.  To update, replace include/ and
src/ with the same directories of the new version and compare the files in
config/ with their new counterparts.

Build
-----

FreeType needs 32-bit int, while the rest of EmuTOS is built with -mshort.
The Makefile therefore builds these sources, and vdi/vdi_ft2.c and
vdi/vdi_ftlibc.c, without -mshort.  The two halves only meet through the
functions declared in vdi/vdi_ft2.h and vdi/vdi_ftos.h.
tools/check-ft-abi.sh checks this.
