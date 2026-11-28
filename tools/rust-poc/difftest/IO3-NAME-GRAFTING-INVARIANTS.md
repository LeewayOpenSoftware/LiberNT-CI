# IO-3 mount-point validator — invariants (authoritative)

Leeway experimental Rust (R1b). The Rust validator `leeway_rs_poc_validate_mount_point`
(`tools/rust-poc/src/reparse_graft.rs`) mirrors the **shipped C** correction
`IopValidateMountPointBuffer` (Stanley, `fix/io3-name-grafting` **`1d45eab8da4`**, after the
overlap rule was dropped). The C fix is the production path and **the contract**; the Rust is
differentially tested against it. The independent C reference oracle here
(`difftest/graft_ref.c`) is a *second transcription* of the same contract — agreement with it
shows the Rust is self-consistent, not that it equals production. The production-equivalence
run links the Rust staticlib into Stanley's host harness (`submodules/host-tests/iomgr`,
`reparse.c` + `Oracle()`); that is the run that gates any future C→Rust swap.

Differential equivalence is **not** proof of correctness. These invariants are the stated
contract; they are checked by a corpus, a boundary-straddling fuzz, a cargo-fuzz target, and
(in-kernel) the D-2 lab leg.

## What it validates
A mount-point `REPARSE_DATA_BUFFER`, i.e. the IO-manager check that must run before
`IopDoNameTransmogrify` does any pointer arithmetic on the substitute/print name ranges. It
closes the IO-3 offset/length defect class by construction (bounds-checked reads, checked
subtraction, no reachable panic). **MOUNT_POINT only** — symlinks are validated separately
(`IopDoSymbolicLinkTransmogrify`).

## Buffer layout
```
 off  size  field
  0    4     ReparseTag          (u32 LE)  -- must be IO_REPARSE_TAG_MOUNT_POINT (0xA0000003)
  4    2     ReparseDataLength   (u16 LE)  -- bytes AFTER this 8-byte fixed header
  6    2     Reserved            (ignored)
  8    8     SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength
                                 (4x u16 LE) -- the MOUNT_POINT field header
 16    .     PathBuffer[]        (ReparseDataLength - 8 bytes)
```
Name offsets/lengths are byte values **relative to PathBuffer** (byte 16).
`path = ReparseDataLength - 8`.

## Return codes
Only two: `STATUS_SUCCESS` (`0x00000000`) and `STATUS_IO_REPARSE_DATA_INVALID`
(`0xC0000278`). A NULL buffer or any non-mount-point tag is `STATUS_IO_REPARSE_DATA_INVALID`
(a single failure code — **not** `INVALID_PARAMETER`, **not** `TAG_NOT_HANDLED`).

## Invariants (SUCCESS iff ALL hold)
- **V1** `buf != NULL`.
- **V2** `len >= 16`.
- **V3** `ReparseTag == IO_REPARSE_TAG_MOUNT_POINT`.
- **V4** `8 <= ReparseDataLength <= 16384 - 8` **and** `ReparseDataLength <= len - 8`.
  `len` is an **upper bound**, not an exact size — the FSCTL/aux buffer may be larger.
- **V5** SubstituteName and PrintName offsets and lengths are all **even** and
  `offset + length <= path` (checked subtraction, no wrap).
- **V6** `SubstituteNameLength >= 2` (non-empty target).
- **V7** the first WCHAR of the substitute name (`PathBuffer + SubstituteNameOffset`) is a
  backslash (`0x005C`) — an absolute NT path.

## Out of scope (deliberately)
- **No overlap rule.** Dropped at `1d45eab8da4`: a non-empty print name that aliases the
  substitute name is legal.
- **No character/surrogate validation.** `IopValidateMountPointBuffer` validates structure
  and ranges, not characters (beyond the single leading backslash of V7).

## Tests
- **D-1 host differential** (`run-differential.sh`): corpus (valid incl. aliased/slack/longer;
  invalid incl. missing-backslash, odd, OOB, wrong-tag, rdl<header, rdl>len-8, len<16, NULL) +
  boundary-straddling fuzz (build-valid-then-perturb). Default 3,000,000 iterations. Passes
  iff 0 disagreements **and** both status classes produced. Negative control: removing V7
  (the backslash check) yields tens of thousands of disagreements.
- **cargo-fuzz** (`fuzz/fuzz_targets/reparse_graft.rs`): libFuzzer differential vs an
  independent in-file oracle; seed corpus in `fuzz/corpus/reparse_graft/`. Linux private CI.
- **D-2 in-kernel** (`IOCTL_LNRSPOC_VALIDATE_MP`, code `0x807`): the same corpus driven
  through `lnrspoc.sys`; input = raw REPARSE_DATA_BUFFER bytes, output = the NTSTATUS. On our
  kernel this is a **load-and-run smoke with free-poison**, not a special-pool Driver Verifier
  run (nvs has no special pool); a real Driver-Verifier+special-pool run needs a Windows 11 VM.
