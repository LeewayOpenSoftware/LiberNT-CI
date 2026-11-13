#!/bin/sh
# Production-equivalence run for the IO-3 mount-point validator: link the SAME Rust source
# into Stanley's shipped host harness (submodules/host-tests/iomgr) as the production symbol
# IopValidateMountPointBuffer, and run it against his Oracle() + fixed cases + generator.
# Also runs the C baseline (reparse.c == Oracle). Both must pass.
#
#   IOMGR  dir with t_reparse.c, reparse.c, ntoskrnl.h (default: the repo's
#          submodules/host-tests/iomgr; override to point at a dropped copy)
#   ITERS  generator iterations (default 200000)
#   CC     host C compiler (default gcc)
# The Rust staticlib is built with the pinned nightly, staged outside the kernel-target
# .cargo config (host triple, prebuilt core), exporting IopValidateMountPointBuffer via the
# graft-diff host crate's alias.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
POC="$(cd "$HERE/.." && pwd)"
IOMGR="${IOMGR:-$POC/../../submodules/host-tests/iomgr}"
ITERS="${ITERS:-200000}"
CC="${CC:-gcc}"
TC="nightly-2026-05-24"
[ -f "$IOMGR/t_reparse.c" ] || { echo "no t_reparse.c under IOMGR=$IOMGR" >&2; exit 2; }

STAGE="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/io3h-$$")"
mkdir -p "$STAGE/tools/rust-poc/src" "$STAGE/tools/rust-poc/difftest"
cp "$POC/src/reparse_graft.rs" "$STAGE/tools/rust-poc/src/"
cp -r "$HERE/graft-diff" "$STAGE/tools/rust-poc/difftest/"
( cd "$STAGE/tools/rust-poc/difftest/graft-diff"
  if command -v rustup >/dev/null 2>&1; then rustup run "$TC" cargo build --release
  else cargo build --release; fi )
RUSTA="$(find "$STAGE" \( -name 'libleeway_rs_graft_diff*.a' -o -name 'leeway_rs_graft_diff*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

echo "== C baseline: reparse.c == Oracle =="
"$CC" -std=c11 -O2 -Wall -I"$IOMGR" "$IOMGR/t_reparse.c" "$IOMGR/reparse.c" -o "$STAGE/t_c"
"$STAGE/t_c" "$ITERS"

echo "== Rust: validate_mount_point linked as IopValidateMountPointBuffer, vs Oracle + fixed cases =="
"$CC" -std=c11 -O2 -Wall -I"$IOMGR" "$IOMGR/t_reparse.c" "$RUSTA" -o "$STAGE/t_rust"
"$STAGE/t_rust" "$ITERS"

rm -rf "$STAGE"
echo "io3 production-equivalence: PASS (C and Rust both agree with the Oracle + fixed cases)"
