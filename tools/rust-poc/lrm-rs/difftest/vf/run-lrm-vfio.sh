#!/bin/sh
# LRM-RS Driver-Verifier Stage 3 (I/O manager) differential: the real vfio.c (hard + soft I/O rules,
# the pended IRP tracker and the VfIrpUntrackSkipped guard, compiled UNMODIFIED against the host
# <ntoskrnl.h> stub, reached via the exported VfIoThunks[] replacement wrappers) vs the Rust
# (lrm_vfio_*). See vfio_diff.cpp.
#   VFIO_C   path to vfio.c (default: vendored dw/vfio.c, pinned feat/special-pool 93a4f23190e)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/../.." && pwd)"            # tools/rust-poc/lrm-rs
CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-1.97.1}"
VFIO_C="${VFIO_C:-$HERE/dw/vfio.c}"
[ -f "$VFIO_C" ] || { echo "vfio.c not found: $VFIO_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo rustc --release --lib --crate-type staticlib )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -Wno-multichar -I"$HERE/iostub" -c -o "$STAGE/vfio.o" "$VFIO_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/iostub" -o "$STAGE/vfio_diff" \
    "$HERE/vfio_diff.cpp" "$STAGE/vfio.o" "$RUSTA"
"$STAGE/vfio_diff"
