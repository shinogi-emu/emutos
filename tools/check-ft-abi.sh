#!/bin/bash
#
# check-ft-abi.sh - check the boundary between 16-bit int and 32-bit int code
#
# FreeType and the outline font engine are built with 32-bit int, the rest
# of EmuTOS with 16-bit int (-mshort).  Calls between the two are only
# safe through the functions declared in vdi/vdi_ft2.h and vdi/vdi_ftos.h,
# which take and return nothing but LONG, ULONG and pointers.
#
# Usage: tools/check-ft-abi.sh [objdir]
#
# This lists every symbol that is referenced by objects of one kind and
# defined by objects of the other kind, apart from the functions declared
# in those two headers.  It prints nothing and exits with 0 if there is
# none; otherwise it prints the symbols, one per line, and exits with 1.
#
# The 32-bit int objects are obj/ft_*.o, obj/vdi_ft2.o and obj/vdi_ftlibc.o
# (see FT_OBJ in the Makefile).  Set NM to override the nm program.
#
# This file is distributed under the GPL, version 2 or at your
# option any later version.  See doc/license.txt for details.
#

export LC_ALL=C

objdir=${1:-obj}
top=$(cd "$(dirname "$0")/.." && pwd)

if [ -z "$NM" ]; then
    NM=nm
    for prefix in m68k-atari-mintelf- m68k-atari-mint- m68k-elf-; do
        if command -v ${prefix}nm >/dev/null 2>&1; then
            NM=${prefix}nm
            break
        fi
    done
fi

wide=()     # objects built with 32-bit int
narrow=()   # all the others
for o in "$objdir"/*.o; do
    case $(basename "$o") in
    ft_*.o|vdi_ft2.o|vdi_ftlibc.o)  wide+=("$o") ;;
    *)                              narrow+=("$o") ;;
    esac
done

if [ ${#wide[@]} -eq 0 ] || [ ${#narrow[@]} -eq 0 ]; then
    echo "check-ft-abi.sh: no objects found in $objdir" >&2
    exit 2
fi

# global symbols defined / undefined by a list of objects, without the
# leading underscore some toolchains add
defined() {
    $NM -P -g --defined-only "$@" 2>/dev/null | awk 'NF >= 3 && $1 !~ /:$/ {print $1}' | sed 's/^_//' | sort -u
}
undefined() {
    $NM -P -g -u "$@" 2>/dev/null | awk 'NF >= 2 && $1 !~ /:$/ {print $1}' | sed 's/^_//' | sort -u
}

# the functions the two headers declare
allowed=$(cat "$top/vdi/vdi_ft2.h" "$top/vdi/vdi_ftos.h" |
          grep -o '\bvdi_[a-z0-9_]*[[:space:]]*(' | tr -d ' (' | sort -u)

wide_def=$(defined "${wide[@]}")
wide_und=$(undefined "${wide[@]}")
narrow_def=$(defined "${narrow[@]}")
narrow_und=$(undefined "${narrow[@]}")

cross=$( { comm -12 <(echo "$narrow_und") <(echo "$wide_def")
           comm -12 <(echo "$wide_und") <(echo "$narrow_def"); } | sort -u )

bad=$(comm -23 <(echo "$cross" | grep -v '^$') <(echo "$allowed"))

if [ -n "$bad" ]; then
    echo "$bad"
    exit 1
fi

exit 0
