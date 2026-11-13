#!/bin/sh
# Native Win11 build of the real shimgvw viewer (MSYS2 clang64, static):
#   PATH=/c/msys64/clang64/bin:$PATH sh dll/win32/shimgvw/standalone/build.sh
# Output: standalone/out/photoviewer.exe (+ icons dir)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SH="$HERE/.."; ROOT="$SH/../../.."; LW="$ROOT/sdk/lib/leewayui"
OUT="$HERE/out"; mkdir -p "$OUT/obj"
INC="-I$HERE/shim -I$SH -I$ROOT/sdk/include -I$ROOT/sdk/include/reactos -I$LW"
DEF="-include $HERE/shim/pre.h -DUNICODE -D_UNICODE -D__REACTOS__ -D_WIN32_WINNT=0x0A00 -DVPILL_TESTHOOK -Wno-everything"
for f in shimgvw util anime vribbon_model vpill; do
  clang -std=gnu11 -O2 $DEF $INC -c "$SH/$f.c" -o "$OUT/obj/$f.o" &
done
for f in loader vribbon; do
  clang++ -std=gnu++17 -O2 -fms-extensions $DEF $INC -c "$SH/$f.cpp" -o "$OUT/obj/$f.o" &
done
for s in ribbon frame dpi theme aureline_icons; do
  clang++ -std=gnu++17 -O2 -fms-extensions $DEF $INC -c "$LW/$s.cpp" -o "$OUT/obj/lw_$s.o" &
done
clang -std=gnu11 -O2 $DEF $INC -c "$HERE/shlwapi_compat.c" -o "$OUT/obj/shlwapi_compat.o" &
clang -std=gnu11 -O2 $DEF $INC -c "$HERE/harness.c" -o "$OUT/obj/harness.o" &
wait
SHW="$(cd "$SH" && pwd -W)"
sed -e '/reactos\/version.rc/d' -e '/manifest_hosted/d' -e 's#<winuser.rh>#<winuser.h>#' \
    -e '/WINE_REGISTRY/d' -e 's#"res/#"'"$SHW"'/res/#g' -e 's#"lang/#"'"$SHW"'/lang/#g' "$SH/shimgvw.rc" > "$OUT/rc_gen.rc"
llvm-windres --codepage=65001 -DLANGUAGE_EN_US -DUNICODE -D_UNICODE -I"$SH" -I"$ROOT/sdk/include" -I"$ROOT/sdk/include/reactos" "$OUT/rc_gen.rc" -O coff -o "$OUT/obj/rsrc.o"
SAW="$(cd "$HERE" && pwd -W)"
printf '1 24 "%s/photoviewer.manifest"
' "$SAW" > "$OUT/manifest.rc"
llvm-windres "$OUT/manifest.rc" -O coff -o "$OUT/obj/manifest.o"
clang++ -static -mwindows -municode -Wl,--allow-multiple-definition -o "$OUT/photoviewer.exe" "$OUT"/obj/*.o \
  -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 \
  -lshlwapi -luuid -luxtheme -ldwmapi -lshcore -lmsimg32 -lwinmm -limm32 -lversion
echo "built $OUT/photoviewer.exe"
mkdir -p "$OUT/icons"; cp -r "$ROOT/sdk/lib/leewayui/icons/viewer" "$OUT/icons/"
