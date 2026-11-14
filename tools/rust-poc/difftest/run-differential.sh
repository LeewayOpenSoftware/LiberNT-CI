#!/bin/sh
# D-1: build the IO-3 name-grafting validator (host staticlib, the SAME source as the
# kernel build) and the differential harness, then run it. Exit 0 iff the C reference and
# the Rust validator agree on every input AND every status class was produced.
#   ITERS  deterministic fuzz iterations (default 3000000)
#   CXX    host C++ compiler (default g++)
#   CC     host C compiler   (default gcc)
# Uses the pinned toolchain via rustup when available.
#
# The lnrspoc crate's .cargo/config.toml forces the custom kernel target + -Zbuild-std on
# everything nested under tools/rust-poc. The host differential must NOT inherit that, so
# we stage the host crate + the shared validator source into a temp tree that has no such
# config, preserving the `#[path = "../../../src/reparse_graft.rs"]` layout, and build
# there for the host triple with the prebuilt core (no build-std).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
POC="$(cd "$HERE/.." && pwd)"               # tools/rust-poc
ITERS="${ITERS:-3000000}"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
TC="1.97.1"

STAGE="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/graft-diff-$$")"
mkdir -p "$STAGE/tools/rust-poc/src" "$STAGE/tools/rust-poc/difftest"
cp "$POC/src/reparse_graft.rs" "$STAGE/tools/rust-poc/src/"
cp -r "$HERE/graft-diff" "$STAGE/tools/rust-poc/difftest/"
CRATE="$STAGE/tools/rust-poc/difftest/graft-diff"
rm -rf "$CRATE/target"

( cd "$CRATE"
  if command -v rustup >/dev/null 2>&1; then
    rustup run "$TC" cargo build --release
  else
    cargo build --release
  fi )

RUSTA="$(ls "$CRATE"/target/release/deps/libleeway_rs_graft_diff-*.a \
            "$CRATE"/target/release/deps/leeway_rs_graft_diff-*.lib \
            "$CRATE"/target/release/libleeway_rs_graft_diff.a 2>/dev/null | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found under $CRATE/target" >&2; exit 2; }

"$CC"  -std=c11   -O2 -Wall -Wextra -c -o "$HERE/graft_ref.o" "$HERE/graft_ref.c"
"$CXX" -std=c++17 -O2 -Wall -Wextra -o "$HERE/graft_differential" \
    "$HERE/graft_differential.cpp" "$HERE/graft_ref.o" "$RUSTA"
"$HERE/graft_differential" "$ITERS"
rc=$?
rm -rf "$STAGE"
exit $rc
