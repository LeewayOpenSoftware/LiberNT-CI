#!/bin/sh
# LRM-RS Driver-Verifier Stage 5 differential: the real vfpool.c (pool checks + tracking + leak at
# unload, compiled UNMODIFIED against the host <ntoskrnl.h> stub, reached via the exported
# VfPoolThunks[] + VfInitializePoolTracking/VfUntrackPool/VfCheckPoolAtUnload) vs the Rust (lrm_vf_*).
#   VFPOOL_C  path to vfpool.c (default: vendored dw/vfpool.c, pinned feat/special-pool 5f5f92ed47a)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"            # tools/rust-poc/lrm-rs
CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-nightly-2026-05-24}"
VFPOOL_C="${VFPOOL_C:-$HERE/dw/vfpool.c}"
[ -f "$VFPOOL_C" ] || { echo "vfpool.c not found: $VFPOOL_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo build --release )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -I"$HERE/poolstub" -c -o "$STAGE/vfpool.o" "$VFPOOL_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/poolstub" -o "$STAGE/vfpool_diff" \
    "$HERE/vfpool_diff.cpp" "$STAGE/vfpool.o" "$RUSTA"
"$STAGE/vfpool_diff"
