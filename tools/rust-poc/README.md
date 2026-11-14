# lnrspoc - experimental Rust kernel-driver PoC (v2)

Owner-approved 2026-11-12 (v1 `kernel/OWNER-OK-RUST-POC-2026-11-12.md`; v2
`kernel/OWNER-OK-RUST-POC-V2-2026-11-12.md`). Establishes a safe, maintainable path to
link Rust into our existing NT kernel **behind a C ABI** - not a kernel rewrite. This is
an experiment: **off by default, never on the ISO, ntoskrnl untouched.**

## What it is
- `leeway_rs_poc` - a `no_std` Rust **staticlib** (no heap, no locks in v1) exposing one
  C-ABI function that validates an untrusted length-prefixed record and returns its
  CRC-32. Every buffer access is bounds-checked; expected failures are **returned**,
  never faulted.
- `lnrspoc.sys` - a minimal WDM driver that links the staticlib and calls it from a
  self-test (on load) and IOCTLs. It is the host for the experiment.

**v2 adds** (owner-approved 2026-11-12), still `no_std`, still off by default:
- a pool-backed `#[global_allocator]` (NT nonpaged pool via `ExAllocatePoolWithTag`) with
  a **fallible** `try_*` / `KBox` surface - allocation failure returns `Err`, never panics
  or bugchecks (no infallible `alloc` path is used anywhere);
- `SpinLock<T>` - RAII over a `KSPIN_LOCK`; acquire raises IRQL to DISPATCH_LEVEL, the
  guard restores it on drop (every scope exit);
- bindgen-style **NDK struct bindings** with compile-time `size_of`/`offset_of!`
  assertions (`src/ndk.rs`), which closes layout-correctness acceptance test 4.

## Layout
```
tools/rust-poc/
  Cargo.toml  Cargo.lock      crate manifest (panic=abort, no registry dependencies)
  rust-toolchain.toml         pinned stable toolchain (= LIBERNT_RUST_TOOLCHAIN)
  .cargo/config.toml          plain `cargo build`: x86_64-pc-windows-gnu + v0 mangling
  src/lib.rs                  crate root (validator, CRC-32, panic->bugcheck, v2 exercisers)
  src/alloc_pool.rs           pool #[global_allocator] + fallible try_*/KBox + OOM probe
  src/sync.rs                 SpinLock<T> RAII over KSPIN_LOCK (IRQL raise/restore)
  src/ndk.rs                  NDK struct bindings + size/offset const_assert (test 4)
  tools/gen-ndk.sh            regenerate src/ndk.rs with bindgen (output is authoritative)
  include/leeway_rs_poc.h      C ABI contract (authoritative; cbindgen can regen)
  driver/lnrspoc.c             WDM host driver
  CMakeLists.txt               add_rust_staticlib() + driver; NO add_cd_file
  .github/workflows/rust-poc-reproducible.yml   build-twice sha256 gate + static audits
```

## Toolchain pin (reproducibility)
- One pinned **stable** release, `LIBERNT_RUST_TOOLCHAIN` in `sdk/cmake/rust.cmake` (today
  **1.97.1**), mirrored in `rust-toolchain.toml`; CI fails if the two differ or if either names a
  nightly or beta. Components: `llvm-tools`; target: `x86_64-pc-windows-gnu`.
- Kernel staticlibs use the **built-in** `x86_64-pc-windows-gnu` target with the toolchain's
  precompiled `core`/`alloc`. No `-Zbuild-std`, no custom target JSON, no `#![feature]`. The old
  JSON target only changed the code model and the red zone, and neither matters for a
  Windows x64 PE image (the Win64 ABI has no red zone).
- `sdk/cmake/rust.cmake` checks the toolchain, target and llvm-tools at configure time and
  defines `add_rust_staticlib()`. The build recipe is `sdk/cmake/rust-build.cmake` (a `cmake -P`
  script, also run by CI):
  - cargo runs `--locked --offline` with a private `CARGO_HOME`, so the user's cargo config
    and the network are never used. A crate that needs a registry or git dependency fails.
  - Every absolute path is remapped (`--remap-path-prefix`) and incremental builds are off.
  - Only the crate's own objects go into the archive the kernel links. The bundled
    `compiler_builtins` members are dropped, so runtime helpers come from the kernel's libraries.
  - The build fails if those objects reference a symbol outside the target's
    `ALLOWED_EXTERNS`, or contain x87, MMX or AVX code.
  - It writes `lib<target>.a.sha256`.
- Reproducible: the same crate built from two source locations into two work directories
  gives byte-identical archives. CI (`reproducible-staticlib` job) checks this on every
  change and runs two negative controls on the extern gate.
- Vendoring: the kernel crates have no third-party dependencies, so there is nothing to
  vendor. If one is ever added, vendor it under `tools/rust-poc/vendor` with `cargo vendor`
  and add a `[source]` replacement; the `--offline` build enforces this.
- `fuzz/` is the exception: cargo-fuzz needs nightly. It is host-only and never linked into
  the kernel.
- To bump: change both pins, run the reproducible job, and record the new hashes.

## Bugcheck code registry (Leeway, experimental)
| Symbol | Value | Meaning | Reachable from |
|---|---|---|---|
| `LEEWAY_POC_RUST_PANIC` | `0x4C450001` | Unrecoverable Rust-side panic in the v1 PoC (controlled bring-down via `KeBugCheckEx`) | ONLY the gated test IOCTL `IOCTL_LNRSPOC_FORCE_PANIC` (`test-ioctls` build, disposable VM) |

> Not to be confused with the **registered LRM-RS** bug check `LEEWAY_RUST_PANIC` =
> `0x4C524D00` (generated from `sdk/include/reactos/mc/bugcodes.mc`), used by the `lrm_rs`
> special-pool panic policy. The v1 PoC code below was renamed from `LEEWAY_RUST_PANIC` to
> `LEEWAY_POC_RUST_PANIC` (2026-11-14) so the registered name means exactly one value: when
> both headers are in scope (e.g. `lnrspoc.c`) the old duplicate tripped
> `-Werror,-Wmacro-redefined`.

`0x4C450001` ("LE" = Leeway, experiment 01) is a **provisional, experimental** code for
**our** kernel — not a Microsoft bugcheck code and **not a public or stable contract**. It
is **exclusive to controlled lnrspoc testing** (owner-approved 2026-11-12): it fires only on
an explicit `IOCTL_LNRSPOC_FORCE_PANIC` in the `test-ioctls` build and **never in normal
driver operation** (the default build has no bugcheck path at all — see "Safety posture"),
and deliberate crashes are run **only in a disposable VM** where the dump diagnostics and KD
Rust-symbol resolution are verified. `P1`/`P2` carry the panic source line/column for `!analyze`.

**Collision check (2026-11-12, tree `b7274436923`):** `0x4C450001` has **zero** occurrences
anywhere in the source tree — no entry in `sdk/include/reactos/mc/bugcodes.mc` (whose NT
bugcheck codes are all small, e.g. `0x117`/`0x121`/`0x141`), none in the KMDF
`sdk/include/wdf/kmdf/*/wdfbugcodes.h`, and it is not a documented Windows bugcheck code. The
earlier candidate `0xE0000001` was rejected because it already defines the NTSTATUS
`STATUS_S1_HASH_MISMATCH` (`base/system/servicing/pending/pending.c:110`) in the adjacent
S1-servicing domain.

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
- `IOCTL_LNRSPOC_V2_SELFTEST` (0x805): runs the pool+SpinLock load self-test on demand;
  output = `int` result (0 = PASS).
- `IOCTL_LNRSPOC_ALLOC_STRESS` (0x802) / `IOCTL_LNRSPOC_LOCK_STRESS` (0x803): input =
  `unsigned long` iteration count, output = `int` result. Allocator / lock churn. Safe,
  non-destructive, present in every build.
- `IOCTL_LNRSPOC_OOM_PROBE` (0x804): deliberately exhausts the nonpaged pool and proves
  failure is a returned `Err`, not a crash; output = `int` block count (>0) or a negative
  code. All blocks are freed. Safe (non-destructive) but pool-heavy: a disposable VM.
- `IOCTL_LNRSPOC_FORCE_PANIC` (0x801): **destructive**, test builds only. Triggers the
  controlled bugcheck. Run ONLY in a disposable VM.

## Safety posture
- **No bugcheck on normal operation** - verified: in the default build the optimizer
  proves nothing on the normal path can panic and removes the panic handler and the
  `KeBugCheckEx` reference entirely. The default build's only undefined externs are the
  **sanctioned** ntoskrnl pool/spin-lock routines (`ExAllocatePoolWithTag`,
  `ExFreePoolWithTag`, `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`,
  `KeReleaseSpinLock`) - **never `KeBugCheckEx`**. The bugcheck path appears only in the
  `test-ioctls` build, which adds exactly `KeBugCheckEx` to that set. Both facts are gated
  in CI (`rust-poc-reproducible.yml`).
- **Allocation failure is a value, not a trap** - the pool allocator's public surface is
  fallible (`try_alloc`, `KBox::try_new` -> `Result`); the OOM probe exhausts the pool and
  observes a returned `Err`. `handle_alloc_error` is never reachable.
- **panic = abort** - no unwinding across the FFI boundary; a panic is an immediate,
  controlled bugcheck, not UB crossing into C frames. No EH tables emitted.
- **No FP/MMX/AVX** in our code path (ABI-permitted scalar SSE is absent too); verified
  by disassembly. Red zone disabled; MMX disabled; SSE kept only as the Win64 ABI
  requires (unused here).
- All `unsafe` is the FFI slice reconstruction, documented with `// SAFETY:`.

See `kernel/RUST-POC-RESULTS.md` for the eight acceptance tests and their status.

## R1b — IO-3 mount-point validator (first real lnrspoc payload)

`src/reparse_graft.rs` exports `leeway_rs_poc_validate_mount_point(buf, len)` (C ABI in
`include/leeway_rs_poc.h`): an alloc-free, lock-free, panic-free validator over an untrusted
mount-point `REPARSE_DATA_BUFFER`, mirroring the shipped C `IopValidateMountPointBuffer`
(Stanley, `fix/io3-name-grafting` `1d45eab8da4`). The C fix is the production path and THE
contract; this Rust is differentially tested against it. Invariants (V1-V7) are authoritative
in `difftest/IO3-NAME-GRAFTING-INVARIANTS.md` and the module doc comment. Returns only
`STATUS_SUCCESS` or `STATUS_IO_REPARSE_DATA_INVALID`. Experimental; off by default.

- **D-1 host differential**: `sh difftest/run-differential.sh` builds the *same* validator
  source as a host staticlib and compares it to an independent C reference
  (`difftest/graft_ref.c`) over a corpus + a boundary-straddling fuzz (default 3,000,000
  iterations). Passes iff 0 disagreements and both status classes were produced. A CI job
  (`io3-differential`) runs it on push. NOTE: this is the author's own reference; the
  production-equivalence run links the Rust into Stanley's host harness
  (`submodules/host-tests/iomgr`, `reparse.c` + `Oracle()`).
- **cargo-fuzz**: `fuzz/fuzz_targets/reparse_graft.rs` is a libFuzzer differential target
  (seed corpus `fuzz/corpus/reparse_graft/`), run on the Linux private CI.
- **D-2 in-kernel**: `IOCTL_LNRSPOC_VALIDATE_MP` (0x807) drives the validator through
  `lnrspoc.sys`; input = raw REPARSE_DATA_BUFFER bytes, output the `NTSTATUS`. On our kernel
  this is a load-and-run smoke with free-poison, not a special-pool Driver Verifier run
  (our nvs has no special pool); see the D-2 runbook.
