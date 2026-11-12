#!/bin/sh
# Direct standalone Win11 build of the ribbon demo (no bison, no CMake):
# g++/MinGW compiles ribbon.cpp + theme.cpp + dpi.cpp + the demo and links Win32.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/.."
INC="$LIB/../../include"
CXX="${CXX:-g++}"
OUT="${1:-$HERE/ribbon_demo.exe}"
"$CXX" -std=c++14 -municode -DUNICODE -D_UNICODE -O2 -Wall -I"$INC" \
    "$LIB/ribbon.cpp" "$LIB/theme.cpp" "$LIB/dpi.cpp" "$HERE/ribbon_demo.cpp" \
    -o "$OUT" \
    -loleacc -luxtheme -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32
echo "built: $OUT"
