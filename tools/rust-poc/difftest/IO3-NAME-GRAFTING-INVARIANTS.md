# IO-3 name-grafting validator — invariants (authoritative)

Leeway experimental Rust (R1b). The validator `leeway_rs_poc_validate_reparse_graft`
(`tools/rust-poc/src/reparse_graft.rs`) and its independent C reference oracle
(`tools/rust-poc/difftest/graft_ref.c`) both implement exactly this specification. The C
reference is replaced by Stanley's R1a `IoIsValidNameGraftingBuffer` once it lands, and the
differential is re-run against his function.

This is **not** a proof of correctness on its own. The invariants below are the stated
contract; they are checked by (1) a hand corpus, (2) a structured field sweep, (3) a
deterministic differential fuzz, and (4) a cargo-fuzz differential target, and will be
checked in-kernel under Driver Verifier (D-2 lab leg).

## What it validates

A `REPARSE_DATA_BUFFER` used for name grafting, i.e. the IO-manager check that must run
before `IopDoNameTransmogrify` / `IopDoSymbolicLinkTransmogrify` do any pointer arithmetic
on the substitute/print name ranges. Today `IoIsValidNameGraftingBuffer`
(`ntoskrnl/io/iomgr/irp.c`) is `UNIMPLEMENTED; return FALSE;` and `IopDoNameTransmogrify`
(`ntoskrnl/io/iomgr/file.c`) reads `MountPointReparseBuffer` without checking
`SubstituteNameOffset + SubstituteNameLength <= ReparseDataLength`. This validator closes
that offset/length arithmetic defect class by construction (bounds-checked reads, checked
subtraction, no reachable panic).

## Buffer layout

```
 off  size  field
  0    4     ReparseTag          (u32 LE)
  4    2     ReparseDataLength    (u16 LE)  -- counts the bytes AFTER this 8-byte header
  6    2     Reserved             (ignored)
  8    .     per-tag field header + PathBuffer[]      (ReparseDataLength bytes)
```

Per-tag field header: MOUNT_POINT (`0xA0000003`) = 8 bytes (`SubstituteNameOffset`,
`SubstituteNameLength`, `PrintNameOffset`, `PrintNameLength`, each u16); SYMLINK
(`0xA000000C`) = 12 bytes (those four u16 + a u32 `Flags`). Name offsets and lengths are
byte values **relative to PathBuffer** (the area after the per-tag field header).

`path_len = ReparseDataLength - field_header_size(tag)`.

## Return codes

| Status | Value | Meaning |
|---|---|---|
| `STATUS_SUCCESS` | `0x00000000` | all invariants hold |
| `STATUS_INVALID_PARAMETER` | `0xC000000D` | `buf` is NULL (caller misuse) |
| `STATUS_IO_REPARSE_TAG_NOT_HANDLED` | `0xC0000279` | tag is not MOUNT_POINT/SYMLINK |
| `STATUS_IO_REPARSE_DATA_INVALID` | `0xC0000278` | any structural/bounds invariant fails |

## Invariants (SUCCESS iff ALL hold), in decision order

The order matters: an input that fails more than one invariant returns the status of the
**first** one it fails, and the C oracle matches that order exactly.

1. **I1** — `buf != NULL`, else `INVALID_PARAMETER`.
2. **I4** — `reparse_tag ∈ { MOUNT_POINT, SYMLINK }`, else `TAG_NOT_HANDLED`.
3. **I2** — `8 <= len <= 16384`, else `IO_REPARSE_DATA_INVALID`.
4. **I3** — embedded `ReparseTag == reparse_tag`, else `IO_REPARSE_DATA_INVALID`.
5. **I5** — `8 + ReparseDataLength == len` (buffer is exactly its declared size), else `IO_REPARSE_DATA_INVALID`.
6. **I6** — `ReparseDataLength >= field_header_size(tag)`, else `IO_REPARSE_DATA_INVALID`.
7. **I7** — `SubstituteNameOffset`, `SubstituteNameLength`, `PrintNameOffset`, `PrintNameLength` are all even (WCHAR-aligned), else `IO_REPARSE_DATA_INVALID`.
8. **I8** — `SubstituteNameOffset <= path_len` and `SubstituteNameLength <= path_len - SubstituteNameOffset` (checked subtraction, no wrap), else `IO_REPARSE_DATA_INVALID`.
9. **I9** — same range check for `PrintName`, else `IO_REPARSE_DATA_INVALID`.
10. **I10** — `SubstituteNameLength != 0` (a graft needs a non-empty target; PrintName may be empty), else `IO_REPARSE_DATA_INVALID`.

## Out of scope (deliberately)

Character-content validation (embedded NUL, UTF-16 surrogate pairing) is **not** part of
`IoIsValidNameGraftingBuffer`: it validates structure and ranges, not characters. The NTFS
on-disk validator (C1, `sdk/lib/fslib/ntfslib_new/rust`) performs the NUL/surrogate checks
on its own path. Keeping content checks out here keeps the differential against Stanley's
range-only C correction clean.

## Tests

- **D-1 host differential** (`run-differential.sh`): corpus (a–q) + structured sweep
  (both tags, path_len 0..64, every substitute offset/length to +4 past the boundary) +
  deterministic xorshift fuzz. Default 3,000,000 fuzz iterations. Passes iff 0
  disagreements **and** all four status classes were produced. Negative control: removing
  I7 from the Rust yields tens of thousands of disagreements (harness is non-vacuous).
- **cargo-fuzz** (`fuzz/fuzz_targets/reparse_graft.rs`): libFuzzer differential vs an
  independent in-file oracle under three tags; seed corpus in `fuzz/corpus/reparse_graft/`.
  Runs on the Linux private CI.
- **D-2 in-kernel** (`IOCTL_LNRSPOC_VALIDATE_GRAFT`, code `0x806`): the same corpus driven
  through `lnrspoc.sys`, under Driver Verifier + special pool + NonPagedPoolNx, must return
  the same status as the kernel C validator with no bugcheck/OOB. Lab leg (Pam mu3j).

---

## R1a mirror — `IopValidateMountPointBuffer` (the swap-equivalent)

Stanley's shipped C fix (`fix/io3-name-grafting` `ff2e252ff4d`) is `NTSTATUS
IopValidateMountPointBuffer(PREPARSE_DATA_BUFFER buf, ULONG len)`. The Rust function
`leeway_rs_poc_validate_mount_point(buf, len)` (`src/reparse_graft.rs`) mirrors it **exactly**
— this, not the broader `validate_name_grafting` above, is the function a future C→Rust swap
would substitute, so it is what the differential gates on. It returns **only**
`STATUS_SUCCESS` or `STATUS_IO_REPARSE_DATA_INVALID`.

SUCCESS iff ALL hold (else `IO_REPARSE_DATA_INVALID`):
- `buf != NULL` (NULL is a malformed buffer here, **not** `INVALID_PARAMETER`);
- `len >= 16`;
- `ReparseTag == IO_REPARSE_TAG_MOUNT_POINT` (0xA0000003) — MOUNT_POINT only;
- `8 <= ReparseDataLength <= 16384 - 8` **and** `ReparseDataLength <= len - 8` (trailing
  slack beyond the declared length is allowed — this differs from `validate_name_grafting`,
  which requires exact `8 + ReparseDataLength == len`);
- with `path = ReparseDataLength - 8`: the substitute and print offsets and lengths are all
  even and `offset + length <= path` (32-bit, no wrap);
- `SubstituteNameLength >= 2`;
- when `PrintNameLength != 0`, `[SubOff, SubOff+SubLen)` and `[PrintOff, PrintOff+PrintLen)`
  do **not** overlap;
- the first WCHAR of the substitute name (`PathBuffer + SubstituteNameOffset`, where
  PathBuffer starts at byte 16) is `L'\'` (0x005C).

Differential (added to `run-differential.sh`): `validate_mount_point` vs an independent C
oracle `graft_ref_validate_mp` over a mount-point corpus + a boundary-straddling fuzz
(build-valid-then-perturb). Latest host run: **mount-point checks=3,000,011, disagreements=0,
SUCCESS=504,759, INVALID=2,495,252**; negative control (drop the backslash check) ⇒ 53,831
disagreements. The **confirming** run links the Rust staticlib into Stanley's own
`submodules/host-tests/iomgr` (`t_reparse.c` Oracle() + `reparse.c`) once those files are
available to me — requested.

D-2 in-kernel: `IOCTL_LNRSPOC_VALIDATE_MP` (0x807) drives `validate_mount_point`; input = the
raw `REPARSE_DATA_BUFFER` bytes, output = the `NTSTATUS`.
