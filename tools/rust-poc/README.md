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
  Cargo.toml  Cargo.lock      crate manifest (+ trim-paths, panic=abort)
  rust-toolchain.toml         pinned nightly (exact date + components)
  .cargo/config.toml          build-std + custom target + v0 mangling
  targets/x86_64-unknown-libernt-kernel.json   custom kernel target spec
  src/lib.rs                  crate root (validator, CRC-32, panic->bugcheck, v2 exercisers)
  src/alloc_pool.rs           pool #[global_allocator] + fallible try_*/KBox + OOM probe
  src/sync.rs                 SpinLock<T> RAII over KSPIN_LOCK (IRQL raise/restore)
  src/ndk.rs                  NDK struct bindings + size/offset const_assert (test 4)
  tools/gen-ndk.sh            regenerate src/ndk.rs with bindgen (output is authoritative)
  include/leeway_rs_poc.h      C ABI contract (authoritative; cbindgen can regen)
  driver/lnrspoc.c             WDM host driver
  CMakeLists.txt               builds staticlib (custom cargo) + driver; NO add_cd_file
  .github/workflows/rust-poc-reproducible.yml   build-twice sha256 gate + static audits
```

## Toolchain pin (reproducibility)
- Channel: **nightly-2026-05-24** (`1.98.0-nightly`, commit
  `423e3d2529999d5e57fa0351783d1292b9219ad4`), components `rust-src`, `llvm-tools`.
- Nightly is required: `-Zbuild-std` rebuilds `core`/`alloc`/`compiler_builtins` for the
  custom target, and `-Zjson-target-spec` loads the target file. Both are unstable as of
  2026. `alloc` is built from v2 on for the pool `#[global_allocator]`.
- `profile.release` uses `panic="abort"`, `lto=true`, `codegen-units=1`, `opt-level="z"`,
  and `trim-paths="all"` so two clean builds are byte-identical (verified: see
  `kernel/RUST-POC-RESULTS.md`). Offline/air-gapped hosts: `cargo vendor` + `--offline
  --frozen --locked` and a cached toolchain tarball.

## Bugcheck code registry (Leeway, experimental)
| Symbol | Value | Meaning | Reachable from |
|---|---|---|---|
| `LEEWAY_RUST_PANIC` | `0x4C450001` | Unrecoverable Rust-side panic (controlled bring-down via `KeBugCheckEx`) | ONLY the gated test IOCTL `IOCTL_LNRSPOC_FORCE_PANIC` (`test-ioctls` build, disposable VM) |

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

## R1b — IO-3 name-grafting validator (first real lnrspoc payload)

`src/reparse_graft.rs` exports `leeway_rs_poc_validate_reparse_graft(buf, len, reparse_tag)`
(C ABI in `include/leeway_rs_poc.h`): an alloc-free, lock-free, panic-free validator over an
untrusted name-grafting `REPARSE_DATA_BUFFER`, mirroring what `IoIsValidNameGraftingBuffer`
must do before `IopDoNameTransmogrify` touches the substitute/print name ranges. The
invariants are authoritative in `difftest/IO3-NAME-GRAFTING-INVARIANTS.md` (and in the
module doc comment). Experimental; off by default; the C correction (R1a) stays the
production path.

- **D-1 host differential**: `sh difftest/run-differential.sh` builds the *same* validator
  source as a host staticlib and compares it to an independent C reference
  (`difftest/graft_ref.c`) over a corpus + a structured field sweep + a deterministic fuzz
  (default 3,000,000 iterations). Passes iff 0 disagreements and every status class was
  produced. A CI job (`io3-differential`) runs it on every push. Negative control: removing
  an invariant from the Rust produces tens of thousands of disagreements.
- **cargo-fuzz**: `fuzz/fuzz_targets/reparse_graft.rs` is a libFuzzer differential target
  (seed corpus `fuzz/corpus/reparse_graft/`), run on the Linux private CI.
- **D-2 in-kernel**: `IOCTL_LNRSPOC_VALIDATE_GRAFT` (0x806) drives the validator through
  `lnrspoc.sys`; input `[ULONG reparse_tag][reparse buffer]`, output the `NTSTATUS`.
  Exercised in a disposable VM under Driver Verifier (lab leg).
