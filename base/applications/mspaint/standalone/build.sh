#!/bin/sh
# Native Win11 build of the real Paint (MSYS2 clang64, static):
#   PATH=/c/msys64/clang64/bin:$PATH sh base/applications/mspaint/standalone/build.sh
# Output: standalone/out/mspaint.exe. Add -DDEBUG_STANDALONE via CXXFLAGS_EXTRA.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
MP="$HERE/.."
ROOT="$MP/../../.."
OUT="$HERE/out"; mkdir -p "$OUT/obj"
LWUI="$ROOT/sdk/lib/leewayui"
CXX=${CXX:-clang++}
INC="-I$MP -I$HERE/shim -I$ROOT/sdk/lib/atl -I$ROOT/sdk/include -I$ROOT/sdk/include/reactos -I$LWUI"
FL="-std=gnu++17 -O2 -fms-extensions -fexceptions -include $HERE/shim/compat.h -DUNICODE -D_UNICODE -DINITGUID -D__REACTOS__ -DLANGUAGE_EN_US -D_WIN32_WINNT=0x0A00 -Wno-everything $CXXFLAGS_EXTRA"
SRCS="canvas dialogs dib drawing fullscreen history main miniature mouse paint_ribbon palette palettemodel registry selectionmodel sizebox textedit toolbox toolsettings toolsmodel"
OBJS=""
for s in $SRCS; do
  $CXX $FL $INC -c "$MP/$s.cpp" -o "$OUT/obj/$s.o" &
  OBJS="$OBJS $OUT/obj/$s.o"
  [ "$(jobs -r | wc -l)" -ge 3 ] && wait -n
done
for s in ribbon frame dpi theme; do
  $CXX $FL $INC -c "$LWUI/$s.cpp" -o "$OUT/obj/lw_$s.o" &
  OBJS="$OBJS $OUT/obj/lw_$s.o"
  [ "$(jobs -r | wc -l)" -ge 3 ] && wait -n
done
wait
# resources: strip ReactOS-only includes into a generated rc
MPW="$(cd "$MP" && pwd -W)"
sed -e 's#<winuser.rh>#<winuser.h>#' -e '/reactos\/version.rc/d' -e "s#\"img/#\"$MPW/img/#g" -e "s#\"lang/#\"$MPW/lang/#g" -e "s#\"mspaint.exe.manifest\"#\"$MPW/mspaint.exe.manifest\"#" "$MP/rsrc.rc" > "$OUT/rsrc_gen.rc"
llvm-windres -DLANGUAGE_EN_US -DUNICODE -D_UNICODE -I"$MP" -I"$ROOT/sdk/include" -I"$ROOT/sdk/include/reactos" "$OUT/rsrc_gen.rc" -O coff -o "$OUT/obj/rsrc.o"
$CXX -static -mwindows -municode -o "$OUT/mspaint.exe" $OBJS "$OUT/obj/rsrc.o" \
  -lgdi32 -luser32 -lcomctl32 -lcomdlg32 -luxtheme -ldwmapi -lshcore -ladvapi32 -lmsimg32 -lshell32 -lole32 -loleaut32 -luuid -lshlwapi -lgdiplus -lhtmlhelp -lrpcrt4 -lversion -limm32 -lwinmm
echo "built $OUT/mspaint.exe"
