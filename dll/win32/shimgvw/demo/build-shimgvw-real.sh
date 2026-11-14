#!/bin/sh
# Build real shimgvw Photo Viewer for Win11 with pill chrome enabled
# Usage: sh build-shimgvw-real.sh
# Runs on libernt-srv via ssh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../../.."
SHIM="$HERE/.."
LWUI="$ROOT/sdk/lib/leewayui"

CXX=${CXX:-clang++}
CC=${CC:-clang}
OUT="$HERE/photoviewer.exe"
OBJ="${TMPDIR:-/tmp}/pv-real.$$"
mkdir -p "$OBJ"

INC="-I$ROOT/sdk/include -I$SHIM -I$HERE/.."

# Compile shimgvw and dependencies
echo "Compiling shimgvw.c..."
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC \
  -c "$SHIM/shimgvw.c" -o "$OBJ/shimgvw.o"

echo "Compiling shimgvw support files..."
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC \
  -c "$SHIM/util.c" -o "$OBJ/util.o"
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC \
  -c "$SHIM/comsup.c" -o "$OBJ/comsup.o"
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC \
  -c "$SHIM/anime.c" -o "$OBJ/anime.o"
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC \
  -c "$SHIM/vribbon_model.c" -o "$OBJ/vribbon_model.o"

echo "Compiling harness and linking..."
$CXX -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -static -mwindows -municode \
  -DUNICODE -D_UNICODE -Wno-deprecated-declarations $INC \
  "$HERE/photoviewer_harness.cpp" "$SHIM/vribbon.cpp" "$LWUI/ribbon.cpp" \
  "$LWUI/frame.cpp" "$LWUI/dpi.cpp" "$LWUI/theme.cpp" \
  "$OBJ/shimgvw.o" "$OBJ/util.o" "$OBJ/comsup.o" "$OBJ/anime.o" "$OBJ/vribbon_model.o" \
  -o "$OUT" \
  -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 \
  -lshlwapi -luuid -luxtheme -ldwmapi

rm -rf "$OBJ"
echo "built $OUT"
sha256sum "$OUT" | tee "$OUT.sha256"
