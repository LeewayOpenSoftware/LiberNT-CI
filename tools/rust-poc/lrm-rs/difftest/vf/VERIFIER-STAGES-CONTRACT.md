# LRM-RS — Driver Verifier contract stages + differential

Leeway experimental (LRM-RS lead). `src/vf.rs` is a **clean-room** Rust of the Driver-Verifier
decision logic the C enforces on a *verified* driver (Dwight's `ntoskrnl/vf/vfirql.c`,
`ntoskrnl/vf/vfpool.c`, `ntoskrnl/nvs/nt/ntdrvmgmt.c`, observed via Ryan's
`OBSERVED-CONTRACTS-SPECIAL-POOL-VERIFIER.md`). OUR code: contracts observed, no source copied
(owner decision 2026-11-14, hybrid). **C reference pinned at `feat/special-pool 5f5f92ed47a`**
(stage 5 / 2a not yet compile-confirmed in CI, so the diff is pinned, not tracking the branch head).

Bug check: `DRIVER_VERIFIER_DETECTED_VIOLATION` (0xC4). Its parameter layout is
`(SubCode, Irql, P3, P4)` — sub-code in **P1** (unlike special pool's 0xC1, which carries the
sub-code in P4). `MI_VERIFIER_IMPLEMENTED_LEVEL = DRIVER_VERIFIER_SPECIAL_POOLING(0x1) |
DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS(0x8) = 0x9` (god's "level 0x9").

## Stage 2a — IRQL rules at the lock/wait calls (DONE)
`vf.rs` mirrors `vfirql.c` exactly. Each rule bug-checks 0xC4 with `(SubCode, Irql, Object, Extra)`:

| Call | Rule | Sub-code | P3 / P4 |
|---|---|---|---|
| `KeAcquireSpinLockAtDpcLevel` | `Irql < DISPATCH` | `0x40` | lock / 0 |
| `KeReleaseSpinLockFromDpcLevel` | `Irql < DISPATCH` | `0x41` | lock / 0 |
| `KeAcquireSpinLockRaiseToDpc` (x64) | `Irql > DISPATCH` | `0x42` | lock / 0 |
| `KeReleaseSpinLock` (x64) | `Irql != DISPATCH` | `0x32` | lock / 0 |
| `ExAcquireFastMutex` | `Irql > APC` | `0x33` | mutex / 0 |
| `ExReleaseFastMutex` | `Irql != APC` | `0x34` | thread->CombinedApcDisable / mutex |
| `KeWaitFor{Single,Multiple}Objects` | at/above DISPATCH, unless a zero-timeout poll at DISPATCH | `0x3B` | object / timeout ptr |

**Full-stack differential** (`vf/run-lrm-vfirql.sh`): compiles `vfirql.c` UNMODIFIED against the
host `<ntoskrnl.h>` stub (`vf/stub/`), reaches the static `Vf*` wrappers through the exported
`VfIrqlThunks[]` table — the kernel's own call path — with a controlled `KeGetCurrentIrql` and
`KeBugCheckEx` captured via `setjmp`/`longjmp`, and compares each to `lrm_vf_*`. Forced `-D_WIN64`
so the x64 thunk set/order (the production config) is fixed on any host. Exhaustive over every IRQL
at each boundary (and each wait timeout shape). Latest: **60 checks, 0 disagreements** (raised 33 /
ok 27). **Negative control:** perturbing the IRQL fed to the Rust side -> 52 disagreements
(non-vacuous). Vendored `vf/dw/vfirql.c` (provenance `feat/special-pool 5f5f92ed47a`).

## Stage 5 — pool checks + tracking + leak at unload (DONE)
`vf.rs` mirrors `vfpool.c`. Pool-check sub-codes (`KeBugCheckEx(0xC4, Code, Irql, PoolType|Pool, Bytes|Block)`):

| Check | Rule | Sub-code | P3 / P4 |
|---|---|---|---|
| allocate | `Bytes == 0` | `0x00` | PoolType / Bytes |
| allocate | paged and `Irql > APC` | `0x01` | PoolType / Bytes |
| allocate | `Irql > DISPATCH` | `0x02` | PoolType / Bytes |
| free | `Irql > DISPATCH` | `0x12` | NonPagedPool(0) / Block |
| free | `Irql > APC` and the block is tracked paged | `0x11` | PagedPool(1) / Block |

- `special_type(Priority)`: low nibble 8 (overrun) -> 0, 9 (underrun) -> 1, else 2.
- `pool2_decode(Flags, Tag)`: `None` (normal allocator) if `Tag==0`, no type bit, or >1 type bit;
  else PagedPool / NonPagedPoolExecute / NonPagedPoolNx, OR'd with the cache-align mask; zero unless
  `POOL_FLAG_UNINITIALIZED`.
- `PoolTracker` (no-heap, caller-provided slice): `track`/`untrack`/`is_tracked_paged`, per-paged
  counters + peaks mirroring `MmVerifierData`, and `VfCheckPoolAtUnload` ->
  `0xC4 (0x62, BaseDllName, 0, LeakedCount)` for blocks whose caller lies in the driver image.

**Full-stack differential** (`vf/run-lrm-vfpool.sh`): compiles `vfpool.c` UNMODIFIED against the
host stub (`vf/poolstub/`) over mock pool/Mm/Ke services, and drives the real paths: the check
sub-codes via the `VfPoolThunks[]` alloc/free wrappers (setjmp/longjmp capture); `special_type` and
the `pool2` POOL_FLAGS decode observed by a recording `MmAllocateSpecialPool`; and the tracking
counters + `VfCheckPoolAtUnload` leak check compared against the Rust `PoolTracker` driven in
lockstep. Latest: **120 checks, 0 disagreements** (raised 29 / ok 16 / pool2-declined 8 /
pool2-decoded 4 / leak 1 / no-leak 1). **Negative control:** perturbing the Rust IRQL/priority/flags
-> 49 disagreements. Vendored `vf/dw/vfpool.c` (provenance `feat/special-pool 5f5f92ed47a`). Both
verifier differentials are chained from `run-lrm-diff.sh`, so the `lrm-equivalence` CI job runs them.

## Stage 1b — VerifyDrivers / verifier APIs (DONE)
`vf.rs` mirrors `ntdrvmgmt.c`:
- `MmIsVerifierEnabled`: active -> `(level, STATUS_SUCCESS)`; else `(0, STATUS_NOT_SUPPORTED)`.
- `MmIsDriverVerifying`: no LDR section -> FALSE; else `Flags & LDRP_IMAGE_VERIFYING`.
- `MmAddVerifierThunks` validation: list not initialised -> `STATUS_NOT_SUPPORTED`; zero thunk count
  -> `STATUS_INVALID_PARAMETER_1`; module-lookup failure / `LoadedImports == -1` / a PristineRoutine
  outside the module -> `STATUS_INVALID_PARAMETER_2`; else `STATUS_SUCCESS`.
- `MiInitializeDriverVerifier` level: empty VerifyDrivers -> not enabled; else
  `Level = (MmVerifyDriverLevel == -1 ? IMPLEMENTED : MmVerifyDriverLevel) & IMPLEMENTED`, clear
  SPECIAL_POOLING if special pool is off; `Level == 0` -> not enabled, else enabled at that level.
  **Pinned at `5f5f92ed47a`, where `MI_VERIFIER_IMPLEMENTED_LEVEL = 0x9`** (SPECIAL_POOLING |
  TRACK_POOL_ALLOCATIONS). At the later head the mask broadened to add IO_CHECKING |
  DEADLOCK_DETECTION — deferred with those unstable stages.
- `MiIsVerifiedDriverName`: space/tab/comma/semicolon-separated list, lone `*` matches all, otherwise
  case-insensitive (ASCII) name equality.

**Full-stack differential** (`vf/run-lrm-vf1b.sh`): compiles `ntdrvmgmt.c` UNMODIFIED against the
host stub (`vf/nt1bstub/`), with `MiLookupDataTableEntry` / `MiIsSpecialPoolEnabled` harness-
controlled. Drives `MmIsDriverVerifying`, `MmAddVerifierThunks` (all five status outcomes),
`MiInitializeDriverVerifier` (level read back via `MmVerifierData.Level` against a sentinel), and
`MmIsVerifierEnabled` in both the inactive (pre-init) and active (post-init) states. Latest:
**44 checks, 0 disagreements** (status S/NS/IP1/IP2 all hit, both booleans), plus the Rust
name-matcher unit-checked 10/10 (the C `MiIsVerifiedDriverName` is `static`, reached only via
`MiVerifyLoadedImage`, so it is unit-checked here rather than differentially). **Negative control**
(perturbed Rust level / flags / module range) -> 19 disagreements. Vendored `vf/dw/ntdrvmgmt.c`
(provenance `feat/special-pool 5f5f92ed47a`). Chained from `run-lrm-diff.sh`.
