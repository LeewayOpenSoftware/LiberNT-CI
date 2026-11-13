#!/bin/sh
# PROJECT:     LiberNT Rust PoC
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Run the ntfslib decoder rig with this crate as the alternative implementation
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# RIG    a LiberNT checkout holding sdk/lib/fslib/ntfslib_new (and lntexpandstorage)
# ITERS  generated cases per seed (default 1000000)
# SEEDS  generator seeds (default "1 2 3")
# TC     Rust toolchain (default nightly-2026-05-24)
#
# The crate is built outside tools/rust-poc, whose .cargo config targets the kernel.
# Passes only when the rig reports ntfslib, model and rust and zero mismatches for
# every seed: agreement on the inputs it ran, not a proof of equivalence.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CRATE="$(cd "$HERE/.." && pwd)"
RIG="${RIG:?set RIG to a LiberNT checkout with sdk/lib/fslib/ntfslib_new}"
ITERS="${ITERS:-1000000}"
SEEDS="${SEEDS:-1 2 3}"
TC="${TC:-nightly-2026-05-24}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/crate"
cp -r "$CRATE/src" "$CRATE/Cargo.toml" "$STAGE/crate/"
( cd "$STAGE/crate" && CARGO_TARGET_DIR="$STAGE/target" rustup run "$TC" cargo build --release )
RUSTA="$STAGE/target/release/libntfs_decode_rs.a"
[ -f "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

cmake -S "$RIG/sdk/lib/fslib/ntfslib_new/linux" -B "$STAGE/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DNTFSLIB_SANITIZE=ON -DNTFSLIB_DIFF_ALT="$RUSTA"
cmake --build "$STAGE/build" --target ntfslib_decode_diff_test
BIN="$(find "$STAGE/build" -type f -name ntfslib_decode_diff_test | head -1)"
[ -n "$BIN" ] || { echo "ntfslib_decode_diff_test not built" >&2; exit 2; }

rc=0
for seed in $SEEDS; do
    echo "== seed $seed, $ITERS iterations"
    "$BIN" --seed "$seed" "$ITERS" > "$STAGE/out.txt" 2>&1 || rc=1
    cat "$STAGE/out.txt"
    grep -qx 'implementations: ntfslib model rust' "$STAGE/out.txt" || { echo "rust implementation not linked" >&2; rc=1; }
    grep -qx '0 mismatches' "$STAGE/out.txt" || rc=1
done
exit $rc
