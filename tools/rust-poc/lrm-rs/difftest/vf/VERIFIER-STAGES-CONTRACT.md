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

## Stage 3 — I/O-manager rules (DONE)
`vf.rs` mirrors `ntoskrnl/vf/vfio.c`. Bug check: `DRIVER_VERIFIER_IOMANAGER_VIOLATION` (0xC9), the
rule sub-code in **P1**. **Pinned at `feat/special-pool 93a4f23190e`** (merged into the kernel branch
at `aa3269b2df8`). Hard rules bug-check `(0xC9, SubCode, A, B, C)` directly:

| Call | Rule | Sub-code | A / B / C |
|---|---|---|---|
| `IoCallDriver` | `Irql > DISPATCH` | `0x10` | Irql / 0 / 0 |
| `IoCallDriver` | `!Irp \|\| Type != IO_TYPE_IRP` | `0x03` | Irp / 0 / 0 |
| `IoCallDriver` | `!Dev \|\| Type != IO_TYPE_DEVICE` | `0x04` | Dev / 0 / 0 |
| `IoCallDriver` return | IRQL changed across the send | `0x05` | Dev / Before / After |
| `IoCompleteRequest` | `Irql > DISPATCH` | `0x0E` | Irql / Irp / 0 |
| `IoCompleteRequest` | status is `STATUS_PENDING` or `-1` | `0x06` | (ULONG_PTR)Status / Irp / 0 |
| `IoCompleteRequest` | a cancel routine is still set | `0x07` | CancelRoutine / Irp / 0 |
| `IoFreeIrp` | `Type != IO_TYPE_IRP` | `0x01` | Irp / 0 / 0 |
| `IoFreeIrp` | queued to a thread (list non-empty) | `0x02` | Irp / 0 / 0 |

Soft (table) rules go through `VfpReportIrpRule`, modelled by `vfio_report` / `vfio_soft_eval`:
**count the hit; if the rule is fatal AND trusted -> `(0xC9, Rule, Caller, Irp, 0)` with NO log line;
else print one log line (first `VF_IRP_LINES_PRINTED` = 64).** `vfio_rule_fatal`: `0x203`, `0x20B`
fatal; `0x205`, `0x209`, `0x310` log-only. `vfio_table_trusted`: the IRP-table rules
(`0x205/0x209/0x20B/0x310`) are trusted only while `VfIrpUntrackSkipped == 0` — so **`0x20B` is
withheld to a log line once an IRP has left above DISPATCH**, even though it is otherwise fatal. The
four IRP-table rules fire only for a **pended** entry (`VfpIsIrpBelow`: the entry exists and its send
returned `STATUS_PENDING`); `0x205`/`0x209` additionally require the caller not to own the current
stack location; `0x203` is cancel-gated with no pended gate. The `Caller` is a return address and MAY
be a kernel address (driver tail-call), so no in-driver-range assumption is made.

**Full-stack differential** (`vf/run-lrm-vfio.sh`, `vfio_diff.cpp`): compiles `vfio.c` UNMODIFIED
against the host stub (`vf/iostub/`), reaches the static `Vf*` wrappers through the exported
`VfIoThunks[]` table, primes a pended entry through a real `IoCallDriver` returning `STATUS_PENDING`,
and compares each violation (bug check captured via `setjmp`/`longjmp`, log lines counted) against
`lrm_vfio_*`. Latest: **17 checks + 70 cap checks, 0 disagreements** (all 10 hard rules raised; soft
`0x203`/`0x20B` fatal with exact params incl. the sign-extended `-1` status; `0x205`/`0x209`/`0x310`
and the withheld `0x20B` as log; the pended gate; the shared 64-line cap with the hit counter kept in
lockstep). **Negative control** (perturb the determining gate fed to the Rust side) -> 13
disagreements. Vendored `vf/dw/vfio.c` (provenance `feat/special-pool 93a4f23190e`). Chained from
`run-lrm-diff.sh`.

## Stage 4 — deadlock / lock-order rules (DONE)
`vf.rs` mirrors the severity decision of `ntoskrnl/vf/vfdeadlock.c`. Bug check:
`DRIVER_VERIFIER_DETECTED_VIOLATION` (0xC4), rule sub-code in **P1**. **Pinned at
`feat/special-pool 93a4f23190e`.** The graph-walk / held-record bookkeeping stays in C (it owns the
graph state); the Rust port is the observable `VfpReportDeadlockRule` decision, `vfdl_report`: count
the hit; if fatal -> `(0xC4, Rule, Lock, Second, Third)` for `WRONG_THREAD`/`DELETED_OWNED`; else one
log line (first `VF_LOCK_LINES_PRINTED` = 64). `vfdl_rule_fatal`: only `0x1004` WRONG_THREAD and
`0x100B` DELETED_OWNED are fatal; `0x1000` SELF, `0x1001` HIERARCHY, `0x1007` UNACQUIRED are log-only.

| Hook | Rule | Sub-code | Fatal | Lock / Second / Third |
|---|---|---|---|---|
| `VfLockAcquiring` (recursion) | SELF | `0x1000` | no | lock / recursive-caller / 0 |
| `VfLockAcquiring` (inverted order) | HIERARCHY | `0x1001` | no | lock / inverted-lock / inverted-caller |
| `VfLockReleasing` (held by another) | WRONG_THREAD | `0x1004` | **yes** | lock / owner-thread / current-thread |
| `VfLockReleasing` (never acquired) | UNACQUIRED | `0x1007` | no | lock / 0 / 0 |
| `VfForgetLocksInRange` (owned in range) | DELETED_OWNED | `0x100B` | **yes** | owned-lock / owner-thread / owned-count |

**Full-stack differential** (`vf/run-lrm-vfdl.sh`, `vfdl_diff.cpp`): compiles `vfdeadlock.c`
UNMODIFIED against the host stub (`vf/dlstub/`), drives the exported lock hooks
(`VfLockAcquiring`/`Acquired`/`Releasing`, `VfForgetLocksInRange`) with a controllable current thread
and stack limits to make each of the five rules fire, and compares per violation against
`lrm_vfdl_report` — the two fatal rules on code 0xC4 and all four params, the three log rules on the
Log outcome. Latest: **6 checks, 0 disagreements** (fatal 2, logged 3, silent 1; the shared hit
counter matches C exactly, 5 == 5). **Negative control** (feed the Rust side the opposite-severity
rule) -> 5 disagreements. Vendored `vf/dw/vfdeadlock.c` (provenance `feat/special-pool 93a4f23190e`).
Chained from `run-lrm-diff.sh`. (The boot-driver verification walk `MiVerifyBootLoadedImages`
`e2962b8329d`/`76a526b8011` is deferred — not ported here, pending a kd*.dll fix.)
