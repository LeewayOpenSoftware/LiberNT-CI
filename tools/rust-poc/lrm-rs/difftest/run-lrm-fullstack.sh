#!/bin/sh
# LRM-RS FULL-STACK differential: the real ntspecial.c orchestration (over the real special.c
# engine + a mock MiGuard provider) vs the Rust orchestration (lrm_mm_*), same provider.
#   NTSPECIAL_C  path to ntspecial.c (default: vendored dw/nt/ntspecial.c)
#   SPECIAL_C    path to special.c   (default: vendored dw/special/special.c)
#   ITERS        fuzz iterations (default 2000000)
# ntspecial.c is compiled UNMODIFIED against the minimal faithful stub header tree in stub/
# (mint.h/expool.h stand-ins + the real poolspecial.h/poolenv.h), so <nvs/nt/mint.h> etc. resolve
# there. special.c is compiled UNMODIFIED against dw/include. The Rust crate is staged outside
# tools/rust-poc (whose .cargo config targets the kernel).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/.." && pwd)"
ITERS="${ITERS:-2000000}"; CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="${TC:-1.97.1}"
NTSPECIAL_C="${NTSPECIAL_C:-$HERE/dw/nt/ntspecial.c}"
SPECIAL_C="${SPECIAL_C:-$HERE/dw/special/special.c}"
[ -f "$NTSPECIAL_C" ] || { echo "ntspecial.c not found: $NTSPECIAL_C" >&2; exit 2; }
[ -f "$SPECIAL_C" ]   || { echo "special.c not found: $SPECIAL_C" >&2; exit 2; }

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
# Rust staticlib (host), outside the kernel .cargo config.
mkdir "$STAGE/crate"; cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/t" rustup run "$TC" cargo build --release )
RUSTA="$(find "$STAGE/t" \( -name 'liblrm_rs*.a' -o -name 'lrm_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

# Compile the real engine + the real glue, both UNMODIFIED.
"$CC"  -std=c11   -O2 -Wall -Wextra -I"$HERE/dw/include" -c -o "$STAGE/special.o"   "$SPECIAL_C"
"$CC"  -std=c11   -O2 -Wall -Wextra -I"$HERE/stub"       -c -o "$STAGE/ntspecial.o" "$NTSPECIAL_C"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/stub" -o "$STAGE/fs_diff" \
    "$HERE/fullstack_diff.cpp" "$STAGE/ntspecial.o" "$STAGE/special.o" "$RUSTA"
"$STAGE/fs_diff" "$ITERS"
