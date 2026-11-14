#!/bin/sh
# LRM-RS differential: the C special-pool engine (special.c) vs the Rust engine, same contract.
#   SPECIAL_C   path to special.c (default: the vendored dw/special/special.c)
#   SPECIAL_H   path to poolspecial.h (default: the vendored dw/include/poolspecial.h)
#   ITERS       fuzz iterations (default 2000000)
# special.c is compiled UNMODIFIED against a host poolenv.h stand-in (difftest/poolenv.h): we
# assemble include/{poolspecial.h,poolenv.h} + special/special.c so special.c's relative
# "../include/poolspecial.h" and poolspecial.h's "poolenv.h" resolve to that pair. The Rust
# crate is built staged outside tools/rust-poc (whose .cargo config targets the kernel).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/.." && pwd)"
ITERS="${ITERS:-2000000}"; CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-nightly-2026-05-24}"
SPECIAL_C="${SPECIAL_C:-$HERE/dw/special/special.c}"
SPECIAL_H="${SPECIAL_H:-$HERE/dw/include/poolspecial.h}"
[ -f "$SPECIAL_C" ] || { echo "special.c not found: $SPECIAL_C" >&2; exit 2; }
[ -f "$SPECIAL_H" ] || { echo "poolspecial.h not found: $SPECIAL_H" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
# Rust staticlib (host), outside the kernel .cargo config.
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo build --release )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

# Assemble the include/ + special/ layout so special.c compiles unmodified with our poolenv.h.
mkdir -p "$STAGE/b/include" "$STAGE/b/special"
cp "$SPECIAL_H" "$STAGE/b/include/poolspecial.h"
cp "$HERE/poolenv.h" "$STAGE/b/include/poolenv.h"
cp "$SPECIAL_C" "$STAGE/b/special/special.c"

"$CC"  -std=c11   -O2 -Wall -Wextra -c -o "$STAGE/special.o" "$STAGE/b/special/special.c"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$STAGE/b/include" -o "$STAGE/sp_diff" \
    "$HERE/sp_diff.cpp" "$STAGE/special.o" "$RUSTA"
"$STAGE/sp_diff" "$ITERS"

# Full-stack orchestration differential: the real ntspecial.c (MmAllocate/FreeSpecialPool over a
# mock MiGuard provider + special.c) vs the Rust orchestration (lrm_mm_*), same provider.
# Uses its own VENDORED, PINNED ntspecial.c + special.c (whose relative includes resolve to the
# host stand-ins) rather than the CI-fetched engine source, which pulls in <ntoskrnl.h>. The engine
# diff above already proves the engine against the live CI special.c; the full-stack targets the
# orchestration glue and is pinned to the vendored C SHA.
echo "[lrm] full-stack orchestration differential:"
# Pin SPECIAL_C/NTSPECIAL_C to the VENDORED copies explicitly: the CI job exports SPECIAL_C as a
# step-level env var (the branch-head engine source, whose relative include pulls in <ntoskrnl.h>),
# which the child shell would otherwise inherit. The explicit assignment overrides that inherited
# value so the full-stack always builds its own pinned, host-buildable sources.
SPECIAL_C="$HERE/dw/special/special.c" NTSPECIAL_C="$HERE/dw/nt/ntspecial.c" \
    ITERS="$ITERS" CC="$CC" CXX="$CXX" TC="$TC" sh "$HERE/run-lrm-fullstack.sh"

# Driver-Verifier stage differentials (vendored, pinned C at feat/special-pool 5f5f92ed47a).
echo "[lrm] Driver-Verifier Stage 2a differential:"
CC="$CC" CXX="$CXX" TC="$TC" sh "$HERE/vf/run-lrm-vfirql.sh"
echo "[lrm] Driver-Verifier Stage 5 differential:"
CC="$CC" CXX="$CXX" TC="$TC" sh "$HERE/vf/run-lrm-vfpool.sh"
echo "[lrm] Driver-Verifier Stage 1b differential:"
CC="$CC" CXX="$CXX" TC="$TC" sh "$HERE/vf/run-lrm-vf1b.sh"
