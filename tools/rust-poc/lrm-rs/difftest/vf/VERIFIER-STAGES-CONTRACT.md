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

## Stage 5 — pool tracking + leak at unload (NEXT)
`vfpool.c`: per-(block) tracking (`VfpTrack`/`VfUntrackPool`), per-paged counters + peaks in
`MmVerifierData`, pool-check IRQL/zero sub-codes (`0x00`/`0x01`/`0x02` allocate, `0x11`/`0x12`
free), `POOL_TYPE`/`POOL_FLAGS` decode, and the unload leak check `VfCheckPoolAtUnload` ->
`0xC4 (0x62, BaseDllName, 0, LeakedCount)`.

## Stage 1b — VerifyDrivers / verifier APIs (NEXT)
`ntdrvmgmt.c`: `MmIsVerifierEnabled`, `MmIsDriverVerifying`, `MmAddVerifierThunks` (buffer
validation), `MiIsVerifiedDriverName` (name-list parse + `*`), the level computation in
`MiInitializeDriverVerifier`.
