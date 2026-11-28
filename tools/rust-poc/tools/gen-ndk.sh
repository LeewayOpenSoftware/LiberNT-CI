#!/bin/sh
# Regenerate the NDK struct bindings in ../src/ndk.rs with bindgen.
#
# The checked-in src/ndk.rs is authoritative (hand-verified against the NT x64 ABI) so
# the reproducible CI build needs no libclang. Run this only to re-derive it from the
# live NDK headers and diff the result; keep the module's size/offset const_assert block,
# which is what actually closes acceptance test 4.
#
# Requirements: bindgen + libclang, and the LiberNT NDK/SDK headers on the include path.
# Usage: SDK_INC=/path/to/sdk/include DDK_INC=/path/to/sdk/include/ddk sh tools/gen-ndk.sh
set -eu

SDK_INC="${SDK_INC:?set SDK_INC to the LiberNT sdk/include directory}"
DDK_INC="${DDK_INC:-$SDK_INC/ddk}"
OUT="$(dirname "$0")/../src/ndk.generated.rs"

cat > /tmp/ndk-wrap.h <<'H'
#define _AMD64_ 1
#include <ntdef.h>
#include <ntbasedef.h>
H

bindgen /tmp/ndk-wrap.h \
  --use-core --ctypes-prefix core::ffi \
  --no-layout-tests=false \
  --allowlist-type 'LIST_ENTRY|SINGLE_LIST_ENTRY|UNICODE_STRING' \
  --allowlist-type 'KSPIN_LOCK|KIRQL' \
  -o "$OUT" \
  -- -target x86_64-pc-windows-gnu -I"$SDK_INC" -I"$DDK_INC"

echo "wrote $OUT"
echo "Now diff against src/ndk.rs and fold in any ABI changes; keep the const_assert block."
