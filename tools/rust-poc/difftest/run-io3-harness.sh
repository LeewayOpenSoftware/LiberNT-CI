#!/bin/sh
# Production-equivalence run for the IO-3 mount-point validator: link the SAME Rust source
# into Stanley's shipped host harness (submodules/host-tests/iomgr) as the production symbol
# IopValidateMountPointBuffer, and run it against his Oracle() + fixed cases + generator.
# Also runs the C baseline (reparse.c == Oracle). Both must pass.
#
#   IOMGR     dir with t_reparse.c + the ntoskrnl.h stand-in (default: the repo's
#             submodules/host-tests/iomgr; override to point at a dropped copy)
#   REPARSE_C the production reparse.c (default: "$IOMGR/reparse.c" if present — a flattened
#             local drop — else the real repo path ntoskrnl/io/iomgr/reparse.c relative to
#             IOMGR, which is submodules/host-tests/iomgr/../../../ntoskrnl/io/iomgr/reparse.c)
#   ITERS     generator iterations (default 200000)
#   CC        host C compiler (default gcc)
# The Rust staticlib is built with the pinned nightly, staged outside the kernel-target
# .cargo config (host triple, prebuilt core), exporting IopValidateMountPointBuffer via the
# graft-diff host crate's alias.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
POC="$(cd "$HERE/.." && pwd)"
IOMGR="${IOMGR:-$POC/../../submodules/host-tests/iomgr}"
ITERS="${ITERS:-200000}"
CC="${CC:-gcc}"
TC="1.97.1"
[ -f "$IOMGR/t_reparse.c" ] || { echo "no t_reparse.c under IOMGR=$IOMGR" >&2; exit 2; }

# Production reparse.c: flattened drop keeps it next to t_reparse.c; the real repo keeps it at
# ntoskrnl/io/iomgr/reparse.c (host-tests/iomgr/CMakeLists uses CMAKE_CURRENT_LIST_DIR/../../..
# as the source root). Resolve both so this works on CI (full checkout) and on a local drop.
REPARSE_C="${REPARSE_C:-}"
if [ -z "$REPARSE_C" ]; then
    if [ -f "$IOMGR/reparse.c" ]; then
        REPARSE_C="$IOMGR/reparse.c"
    else
        REPARSE_C="$IOMGR/../../../ntoskrnl/io/iomgr/reparse.c"
    fi
fi
[ -f "$REPARSE_C" ] || { echo "reparse.c not found (IOMGR=$IOMGR, REPARSE_C=$REPARSE_C)" >&2; exit 2; }

STAGE="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/io3h-$$")"
mkdir -p "$STAGE/tools/rust-poc/src" "$STAGE/tools/rust-poc/difftest"
cp "$POC/src/reparse_graft.rs" "$STAGE/tools/rust-poc/src/"
cp -r "$HERE/graft-diff" "$STAGE/tools/rust-poc/difftest/"
( cd "$STAGE/tools/rust-poc/difftest/graft-diff"
  if command -v rustup >/dev/null 2>&1; then rustup run "$TC" cargo build --release
  else cargo build --release; fi )
RUSTA="$(find "$STAGE" \( -name 'libleeway_rs_graft_diff*.a' -o -name 'leeway_rs_graft_diff*.lib' \) | head -1)"
[ -n "$RUSTA" ] || { echo "rust staticlib not found" >&2; exit 2; }

echo "== C baseline: reparse.c == Oracle ($REPARSE_C) =="
"$CC" -std=c11 -O2 -Wall -I"$IOMGR" "$IOMGR/t_reparse.c" "$REPARSE_C" -o "$STAGE/t_c"
"$STAGE/t_c" "$ITERS"

echo "== Rust: validate_mount_point linked as IopValidateMountPointBuffer, vs Oracle + fixed cases =="
"$CC" -std=c11 -O2 -Wall -I"$IOMGR" "$IOMGR/t_reparse.c" "$RUSTA" -o "$STAGE/t_rust"
"$STAGE/t_rust" "$ITERS"

rm -rf "$STAGE"
echo "io3 production-equivalence: PASS (C and Rust both agree with the Oracle + fixed cases)"
