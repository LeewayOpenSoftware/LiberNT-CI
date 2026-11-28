#!/bin/sh
# LRM-RS Driver-Verifier Stage 4 (deadlock / lock-order) differential: the real vfdeadlock.c lock
# engine (compiled UNMODIFIED against the host <ntoskrnl.h> stub, driven through the exported lock
# hooks VfLockAcquiring/Acquired/Releasing + VfForgetLocksInRange) vs the Rust severity decision
# (lrm_vfdl_*). See vfdl_diff.cpp.
#   VFDL_C   path to vfdeadlock.c (default: vendored dw/vfdeadlock.c, pinned feat/special-pool 93a4f23190e)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"            # tools/rust-poc/lrm-rs
CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-1.97.1}"
VFDL_C="${VFDL_C:-$HERE/dw/vfdeadlock.c}"
[ -f "$VFDL_C" ] || { echo "vfdeadlock.c not found: $VFDL_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo rustc --release --lib --crate-type staticlib )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -Wno-multichar -I"$HERE/dlstub" -c -o "$STAGE/vfdl.o" "$VFDL_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/dlstub" -o "$STAGE/vfdl_diff" \
    "$HERE/vfdl_diff.cpp" "$STAGE/vfdl.o" "$RUSTA"
"$STAGE/vfdl_diff"
