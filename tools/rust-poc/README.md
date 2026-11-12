# lnrspoc - experimental Rust kernel-driver PoC (v1)

Owner-approved 2026-11-12 (`kernel/OWNER-OK-RUST-POC-2026-11-12.md`). Establishes a
safe, maintainable path to link Rust into our existing NT kernel **behind a C ABI** -
not a kernel rewrite. This is an experiment: **off by default, never on the ISO,
ntoskrnl untouched.**

## What it is
- `leeway_rs_poc` - a `no_std` Rust **staticlib** (no heap, no locks in v1) exposing one
  C-ABI function that validates an untrusted length-prefixed record and returns its
  CRC-32. Every buffer access is bounds-checked; expected failures are **returned**,
  never faulted.
- `lnrspoc.sys` - a minimal WDM driver that links the staticlib and calls it from a
  self-test (on load) and an IOCTL. It is the host for the experiment.

## Layout
```
tools/rust-poc/
  Cargo.toml  Cargo.lock      crate manifest (+ trim-paths, panic=abort)
  rust-toolchain.toml         pinned nightly (exact date + components)
  .cargo/config.toml          build-std + custom target + v0 mangling
  targets/x86_64-unknown-libernt-kernel.json   custom kernel target spec
  src/lib.rs                  the crate (validator, CRC-32, panic->bugcheck)
  include/leeway_rs_poc.h      C ABI contract (authoritative; cbindgen can regen)
  driver/lnrspoc.c             WDM host driver
  CMakeLists.txt               builds staticlib (custom cargo) + driver; NO add_cd_file
  .github/workflows/rust-poc-reproducible.yml   build-twice sha256 gate + static audits
```

## Toolchain pin (reproducibility)
- Channel: **nightly-2026-05-24** (`1.98.0-nightly`, commit
  `423e3d2529999d5e57fa0351783d1292b9219ad4`), components `rust-src`, `llvm-tools`.
- Nightly is required: `-Zbuild-std` rebuilds `core`/`compiler_builtins` for the custom
  target, and `-Zjson-target-spec` loads the target file. Both are unstable as of 2026.
- `profile.release` uses `panic="abort"`, `lto=true`, `codegen-units=1`, `opt-level="z"`,
  and `trim-paths="all"` so two clean builds are byte-identical (verified: see
  `kernel/RUST-POC-RESULTS.md`). Offline/air-gapped hosts: `cargo vendor` + `--offline
  --frozen --locked` and a cached toolchain tarball.

## Bugcheck code registry (Leeway, experimental)
| Symbol | Value | Meaning | Reachable from |
|---|---|---|---|
| `LEEWAY_RUST_PANIC` | `0xE0000001` | Unrecoverable Rust-side panic (controlled bring-down via `KeBugCheckEx`) | ONLY the gated test IOCTL `IOCTL_LNRSPOC_FORCE_PANIC` (test builds, disposable VM) |

These are **our** codes for **our** kernel (not Microsoft bugcheck codes). `0xE0000001`
is a provisional Leeway experimental value; the owner assigns the final reserved code.
`P1`/`P2` carry the panic source line/column to aid `!analyze`.

## Build
Off by default. Enable only via CI or an explicit configure:
```
cmake -DLIBERNT_ENABLE_RUST=ON ...        # builds lnrspoc.sys (not placed on the ISO)
cmake -DLIBERNT_ENABLE_RUST=ON -DLNRSPOC_TEST_IOCTLS=ON ...   # adds the destructive panic IOCTL (VM only)
```
Standalone staticlib (what CI gates):
```
cd tools/rust-poc && cargo build --release --locked
```

## Test IOCTLs (`\.\Lnrspoc`)
- `IOCTL_LNRSPOC_VALIDATE` (0x800, METHOD_BUFFERED): input = record bytes, output =
  `unsigned long` CRC-32. Returns `STATUS_SUCCESS` + CRC, or `STATUS_DATA_ERROR` on
  malformed input (never faults).
- `IOCTL_LNRSPOC_FORCE_PANIC` (0x801): **destructive**, test builds only. Triggers the
  controlled bugcheck. Run ONLY in a disposable VM.

## Safety posture
- **No bugcheck on normal operation** - verified: in the default build the optimizer
  proves `validate()` cannot panic and removes the panic handler and the `KeBugCheckEx`
  reference entirely (the staticlib has *zero* undefined externs). The bugcheck path
  exists only in the `test-ioctls` build, whose sole undefined extern is `KeBugCheckEx`.
- **panic = abort** - no unwinding across the FFI boundary; a panic is an immediate,
  controlled bugcheck, not UB crossing into C frames. No EH tables emitted.
- **No FP/MMX/AVX** in our code path (ABI-permitted scalar SSE is absent too); verified
  by disassembly. Red zone disabled; MMX disabled; SSE kept only as the Win64 ABI
  requires (unused here).
- All `unsafe` is the FFI slice reconstruction, documented with `// SAFETY:`.

See `kernel/RUST-POC-RESULTS.md` for the eight acceptance tests and their status.
