#!/bin/sh
# Native Win11 build of the Paint ribbon demo. MSYS2 clang64, static:
#   PATH=/c/msys64/clang64/bin:$PATH sh base/applications/mspaint/ribbon_demo/build.sh
# Output: paint_ribbon_demo.exe next to this script.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../../.."
LWUI="$ROOT/sdk/lib/leewayui"
CXX=${CXX:-clang++}
$CXX -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -static -mwindows -municode -DUNICODE -D_UNICODE \
  -I"$ROOT/sdk/include" -I"$HERE/.." \
  "$HERE/paint_ribbon_demo.cpp" "$LWUI/ribbon.cpp" "$LWUI/frame.cpp" "$LWUI/dpi.cpp" "$LWUI/theme.cpp" \
  -o "$HERE/paint_ribbon_demo.exe" \
  -lgdi32 -luser32 -lcomctl32 -lcomdlg32 -luxtheme -ldwmapi -lshcore -ladvapi32 -lmsimg32 -lshell32 -lole32
echo "built $HERE/paint_ribbon_demo.exe"
