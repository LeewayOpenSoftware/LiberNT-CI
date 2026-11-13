# Differential test of the NTFS on-disk decoders

`ntfslib_decode_diff_test` feeds the same untrusted bytes to every linked
implementation of three decoders and fails on any disagreement:

| Decoder | Library entry point | ABI entry |
|---|---|---|
| Mapping pairs of a nonresident attribute | `NtfsDecodeMappingPairs` (src/filerecord/attributes/findattr.cpp) | `decode_mapping_pairs` |
| Update sequence fixup of a FILE/INDX record | `NtfsApplyFixup` (src/filerecord/fixup.cpp) | `apply_fixup` |
| Directory ($I30) index entry check | `NtfsIsDirectoryIndexEntryValid` (src/btree/directory/directory.cpp) | `index_entry_valid` |

Implementations: `ntfslib` (the code the driver runs), `model` (model.c,
written from the on-disk format with byte reads only) and, optionally, `alt`.

**The reference is `ntfslib`, the shipped C++.** `model` is a second
transcription of the same rules. It catches a rig or generator that stops
exercising a rule, but agreeing with it proves nothing beyond that. An `alt`,
such as the Rust port, is held to `ntfslib`.

## Inputs

- **Corpus:** three fresh volumes formatted in memory (512/8, 512/1, 4096/1).
  Every FILE record, every INDX block of a nonresident `$INDEX_ALLOCATION`,
  every nonresident attribute and every `$I30` entry they hold.
- **Edges:** hand-written mapping pairs (negative lengths and deltas, 8-byte
  fields, sizes 9, runs past the volume, a missing terminator, VCN mismatches,
  the empty attribute with LastVCN = -1).
- **Generator:** valid inputs built from random runs, records and entries,
  then mutated: flips, truncation, insertion, and header-field changes. It is
  seeded and deterministic (`--seed`).
- **Replay:** `--corpus DIR` loads `mp-*.bin` (8-byte LE cluster count, then
  the attribute record), `fx-*.bin` (4-byte LE bytes per sector, then the
  record) and `ie-*.bin` (the entry area). `--write-corpus DIR` writes the
  built-in corpus in that format, for example as a cargo-fuzz seed corpus.
  `--dump DIR` saves each mismatch the same way.

Each input is copied into a heap block of its exact length, so ASan reports
any read past it. Two outcomes count as equal when they have the same status
and, on success, the same run list (or the same fixed-up record). The run
fails if any kind of input is always accepted or always rejected.

## Invariants (the contract every implementation must keep)

Agreement between implementations is not proof of correctness. These are the
properties the library promises. The model encodes them a second time, and an
alternative must keep them by construction.

### Mapping pairs: `(attr, avail, clusters)`

- **M1.** Reads only `[attr, attr + min(avail, Length))`, and nothing at all
  when `avail < 0x40`, which is then corrupt.
- **M2.** The `avail < 0x40` check of M1 comes first. After it, if the
  nonresident byte (0x08) is 0, the result is `STATUS_INVALID_PARAMETER` and
  nothing else is read. A caller in the library never passes a resident
  attribute: `DecodeDataRuns` returns NULL before calling the decoder.
- **M3.** The header is 0x48 when `Flags & 0x80FF` (compressed or sparse),
  0x40 otherwise. The following must hold:
  - `Length <= avail`;
  - `Length >= header`;
  - `header <= DataRunsOffset < Length`.
- **M4.** Each pair starts with a byte `H`, where `L = H & 0xF` and
  `O = H >> 4`. It requires `1 <= L <= 8`, `O <= 8`, and `1 + L + O` bytes
  before `Length`.
- **M5.** The run length is the `L`-byte little-endian signed value, and it
  must be `> 0`.
- **M6.** The sum of run lengths never wraps in 64 bits.
- **M7.** `O == 0` is a sparse run, with LCN reported as 0. Otherwise:
  - `LCN = previous + signed delta`, and `previous` starts at 0 and moves
    only on non-sparse runs;
  - the LCN never goes below 0 and never wraps above 2^64-1;
  - `LCN < clusters` and `length <= clusters - LCN`.
- **M8.** A 0x00 byte inside `[DataRunsOffset, Length)` ends the list. Without
  one the attribute is corrupt.
- **M9.** `LastVCN >= FirstVCN`, and the sum of lengths equals
  `LastVCN - FirstVCN + 1`, modulo 2^64. That makes an empty list with
  `FirstVCN = 0` and `LastVCN = -1` valid, as NTFS writes it.
- **M10.** Runs reach the sink in on-disk order, before the final checks. A
  sink failure stops the decode and is returned unchanged.
- **M11.** Allocation-free and lock-free, and it never panics or raises on any
  input. It also runs on paging I/O paths.

### Fixup: `(record, size, bps)`

- **F0.** A `bps` below 512 or not a power of two returns
  `STATUS_INVALID_PARAMETER`. The volume probe admits only 512 and 4096, but
  the decoder no longer relies on that: 0 would divide by zero and, without
  the F1 checks, 1 would write below the record.
- **F1.** `size >= bps`, `size % bps == 0`, the USA count equals
  `size / bps + 1`, the USA offset is `<= size`, and `2 * count` fits after it.
- **F2.** For sectors `i = 1 .. count-1`, the last two bytes of sector `i`
  must equal `USA[0]`, read again each time, and are replaced by `USA[i]`, in
  that order and in place.
- **F3.** No access outside `[record, record + size)`. After a failure, the
  buffer contents are unspecified.
- **F4.** Defined, not refused: the update sequence array may overlap the
  record header or a sector tail. F2's in-place order then decides the result,
  and every implementation must follow that order. Refusing such records
  would be stricter than anything known about Windows, so it would need
  evidence first.

### Directory index entry: `(entry, remaining)`

- **I1.** `remaining >= 16`. `EntryLength` must be at least 16, a multiple of
  8, and `<= remaining`. The flags must be within `NODE | END`.
- **I2.** The payload is `EntryLength - 16`, minus 8 for the VCN of a `NODE`
  entry, which must fit. `StreamLength <= payload`.
- **I3.** An `END` entry has `StreamLength == 0`. Any other entry has
  `StreamLength >= 0x42`, and `2 * NameLength <= StreamLength - 0x42`.
- **I4.** No read beyond `remaining`, and no read of the name unless I1 to I3
  allow it.

## Plugging in an alternative (for example the Rust port)

Build a static library exporting
`const ntfs_decode_impl *ntfs_alt_impl(void)`, declared in decode_diff.h.
Leave a function pointer NULL for a decoder you do not implement. Then
configure the host tests with:

```
cmake -S sdk/lib/fslib/ntfslib_new/linux -B build/ntfslib-linux -G Ninja \
      -DNTFSLIB_SANITIZE=ON -DNTFSLIB_DIFF_ALT=/path/to/libntfs_decode_rs.a
cmake --build build/ntfslib-linux --target ntfslib_decode_diff_test
ctest --test-dir build/ntfslib-linux -R decode_diff --output-on-failure
```

A Rust staticlib may also need `-lpthread`. Add it with
`target_link_libraries` when the toolchain asks for it.

## Proposed first decoder for the experimental Rust port

**Mapping pairs (`NtfsDecodeMappingPairs`)**:

- Most of the bounds and arithmetic risk of the three is here: variable-width
  signed fields, two different overflow directions, volume-bound checks, and
  an untrusted terminator.
- It is where the 2026 run-list defects were (corrupt-runlist dirty
  tracking).
- It is a pure function: a byte slice in, runs out through a callback. It
  needs no allocator in Rust (`&[u8]`, `checked_add`, `i64` from LE bytes)
  and no lock, so it fits the lnrspoc v1 shape.
- The other two are worse candidates. The fixup is about a dozen lines of
  in-place writes with little to gain. The index entry check is a few
  comparisons. Either is a fine warm-up for checking the toolchain and the
  link against this rig, but neither earns the FFI boundary in production.
