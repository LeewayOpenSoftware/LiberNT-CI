#!/bin/sh
# Win7 Photo Viewer V0 on LeewayUI. MSYS2 clang64, static:
#   PATH=/c/msys64/clang64/bin:$PATH sh dll/win32/shimgvw/demo/build.sh   (from the repo root, or anywhere)
# Output: photoviewer.exe next to this script. Run: photoviewer.exe [picture]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../../.."
SHIM="$HERE/.."
LWUI="$ROOT/sdk/lib/leewayui"
CXX=${CXX:-clang++}; CC=${CC:-clang}
OUT="$HERE/photoviewer.exe"
OBJ="${TMPDIR:-/tmp}/pv.$$"; mkdir -p "$OBJ"
INC="-I$ROOT/sdk/include -I$SHIM"
$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE $INC -c "$SHIM/vribbon_model.c" -o "$OBJ/model.o"
$CXX -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -static -mwindows -municode -DUNICODE -D_UNICODE -Wno-deprecated-declarations $INC \
  "$HERE/viewer_demo.cpp" "$SHIM/vribbon.cpp" "$LWUI/ribbon.cpp" "$LWUI/frame.cpp" "$LWUI/dpi.cpp" "$LWUI/theme.cpp" \
  "$OBJ/model.o" -o "$OUT" \
  -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -lshlwapi -luuid -luxtheme -ldwmapi
rm -rf "$OBJ"
echo "built $OUT"
