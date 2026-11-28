# R1c — NtfsDecodeMappingPairs (Rust port): invariants + test

Leeway experimental Rust (R1c). `decode_mapping_pairs` in `src/lib.rs` ports the shipped C
`NtfsDecodeMappingPairs` (the rig's **reference**) and the independent `model.c`. It is
`no_std`, alloc-free, lock-free, panic-free (M11). Exported for the ntfslib host-test rig as
`ntfs_alt_impl()` (decode_diff.h); only `decode_mapping_pairs` is set, the other two decoders
are NULL (skipped).

## ABI
`int32_t decode_mapping_pairs(const uint8_t *attr, uint32_t avail, uint64_t clusters_in_volume,
ntfs_diff_run_sink sink, void *context)` — runs reported to `sink(ctx, lcn, length, is_sparse)`
in on-disk order. Status: `SUCCESS 0`, `INVALID_PARAMETER 0xC000000D`, `FILE_CORRUPT 0xC0000102`.

## Invariants (M1–M11, from decode_diff.md — authoritative)
- **M1** reads only `[attr, attr+min(avail,Length))`; nothing when `avail < 0x40` (corrupt).
- **M2** nonresident byte (0x08)==0 → `INVALID_PARAMETER`, nothing else read (checked after M1).
- **M3** header = 0x48 if `Flags & 0x80FF` else 0x40; require `Length<=avail`, `Length>=header`,
  `header <= DataRunsOffset < Length`.
- **M4** each pair head `H`: `L=H&0xF`, `O=H>>4`; `1<=L<=8`, `O<=8`, `1+L+O` bytes before `Length`.
- **M5** run length = `L`-byte signed LE, must be `> 0` (sign bit clear).
- **M6** sum of run lengths never wraps 64-bit.
- **M7** `O==0` sparse (LCN 0); else `LCN = previous + signed O-byte delta`, never below 0 or
  above 2^64-1, and `LCN < clusters`, `length <= clusters - LCN`.
- **M8** a 0x00 byte in `[DataRunsOffset, Length)` ends the list; without one → corrupt.
- **M9** `LastVCN >= FirstVCN` and `sum == LastVCN - FirstVCN + 1` (mod 2^64); empty list with
  `FirstVCN=0, LastVCN=-1` is valid.
- **M10** runs reach the sink in order, before the final checks; a sink failure stops and is
  returned unchanged.
- **M11** alloc-free, lock-free, never panics/raises on any input; runs on paging-I/O paths.

By construction: every field read is bounds-checked (`rd_le` → `Option`), every sum is
`checked_add`, the signed length/delta handling is explicit, one `unsafe` (`from_raw_parts`).

## Tests
- **Local differential** (`run-mp-diff.sh`): Rust vs `model.c` over three volumes (64 / 4096 /
  2^20 clusters), a hand corpus (empty list, resident→INVALID, avail<0x40, missing terminator,
  `ls=9`, run past volume, negative length, **sign-boundary lengths 0x80…**, slack, compressed)
  and a boundary-straddling fuzz (build-valid-then-perturb). Latest: **1,500,051 checks, 0
  disagreements**, SUCCESS/CORRUPT/INVALID all produced. **Negative controls:** removing the M5
  sign check → 24 disagreements (the sign-boundary corpus makes it bite). This is a local
  rust-vs-model check (two transcriptions); the production run is **rust == ntfslib(C++) ==
  model** in the rig.
- **Rig (production reference)**: build this crate as a host staticlib and
  `-DNTFSLIB_DIFF_ALT=<libntfs_decode_rs.a>`; `ntfslib_decode_diff_test` (ASan+UBSan) runs
  lib/model/alt and `--dump <dir>` writes any mismatch. `--write-corpus <dir>` gives a
  cargo-fuzz seed corpus. Run id to be recorded once run.

## Build note
Within `tools/rust-poc/`, a plain `cargo build` inherits the lnrspoc `.cargo/config.toml`
(custom kernel target + `-Zbuild-std`), so the host differential stages the crate outside that
tree (`run-mp-diff.sh`). For the rig, build for a host/Linux target (prebuilt core) — e.g. drop
the crate into the rig tree, or build staged, and point `-DNTFSLIB_DIFF_ALT` at the `.a`.
