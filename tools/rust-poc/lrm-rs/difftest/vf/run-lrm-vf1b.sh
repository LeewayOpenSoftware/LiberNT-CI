#!/bin/sh
# LRM-RS Driver-Verifier Stage 1b differential: the real ntdrvmgmt.c verifier APIs (compiled
# UNMODIFIED against the host <ntoskrnl.h> stub) vs the Rust (lrm_vf_*).
#   NTDRVMGMT_C  path to ntdrvmgmt.c (default: vendored dw/ntdrvmgmt.c, pinned 5f5f92ed47a)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"            # tools/rust-poc/lrm-rs
CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-1.97.1}"
NTDRVMGMT_C="${NTDRVMGMT_C:-$HERE/dw/ntdrvmgmt.c}"
[ -f "$NTDRVMGMT_C" ] || { echo "ntdrvmgmt.c not found: $NTDRVMGMT_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo rustc --release --lib --crate-type staticlib )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -I"$HERE/nt1bstub" -c -o "$STAGE/ntdrvmgmt.o" "$NTDRVMGMT_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/nt1bstub" -o "$STAGE/vf1b_diff" \
    "$HERE/vf1b_diff.cpp" "$STAGE/ntdrvmgmt.o" "$RUSTA"
"$STAGE/vf1b_diff"
