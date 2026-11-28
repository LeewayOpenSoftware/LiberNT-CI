#!/bin/sh
# Local R1c differential: Rust decode_mapping_pairs vs model.c, over a generator + edges.
# Stages the crate outside the kernel-target .cargo config (host triple, prebuilt core).
#   ITERS fuzz iterations (default 2000000)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/.." && pwd)"
ITERS="${ITERS:-2000000}"; CXX="${CXX:-g++}"; CC="${CC:-gcc}"; TC="1.97.1"
STAGE="$(mktemp -d)"; mkdir -p "$STAGE/ntfs-decode-rs"
cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/ntfs-decode-rs/"
export CARGO_TARGET_DIR="$STAGE/cargotgt"
( cd "$STAGE/ntfs-decode-rs"
  if command -v rustup >/dev/null 2>&1; then rustup run "$TC" cargo build --release
  else cargo build --release; fi )
RUSTA="$(find "$STAGE" \( -name 'libntfs_decode_rs*.a' -o -name 'ntfs_decode_rs*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }
"$CC"  -std=c11   -O2 -Wall -Wextra -c -o "$HERE/mp_model.o" "$HERE/mp_model.c"
"$CXX" -std=c++17 -O2 -Wall -Wextra -o "$HERE/mp_diff" "$HERE/mp_diff.cpp" "$HERE/mp_model.o" "$RUSTA"
"$HERE/mp_diff" "$ITERS"; rc=$?
rm -rf "$STAGE"; exit $rc
