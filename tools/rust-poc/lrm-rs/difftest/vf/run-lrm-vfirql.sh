#!/bin/sh
# LRM-RS Driver-Verifier Stage 2a differential: the real vfirql.c IRQL rules (compiled UNMODIFIED
# against the host <ntoskrnl.h> stub, reached via the exported VfIrqlThunks[]) vs the Rust rules.
#   VFIRQL_C  path to vfirql.c (default: vendored dw/vfirql.c, pinned feat/special-pool 5f5f92ed47a)
# Forced -D_WIN64 so the x64 thunk set + order is fixed on any host (the production config).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"            # tools/rust-poc/lrm-rs
CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-nightly-2026-05-24}"
VFIRQL_C="${VFIRQL_C:-$HERE/dw/vfirql.c}"
[ -f "$VFIRQL_C" ] || { echo "vfirql.c not found: $VFIRQL_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo build --release )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -D_WIN64 -I"$HERE/stub" -c -o "$STAGE/vfirql.o" "$VFIRQL_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -D_WIN64 -I"$HERE/stub" -o "$STAGE/vfirql_diff" \
    "$HERE/vfirql_diff.cpp" "$STAGE/vfirql.o" "$RUSTA"
"$STAGE/vfirql_diff"
