# LRM-RS — Special Pool engine contract + differential

Leeway experimental (LRM-RS lead). `src/lib.rs` is a **clean-room** Rust implementation of the
SAME NT special-pool ENGINE contract as Dwight's C (`nvs/pool/special/special.c` +
`poolspecial.h`, `feat/special-pool a42b006aab7`). OUR code: ARM3/NT behaviour and contracts may
be read, never copied (owner decision 2026-11-14, hybrid). The C engine is the differential
reference and the in-kernel fallback; a production swap needs the owner's OK. `no_std`, no heap,
lock-free, panic-free.

## Constants
PAGE_SIZE 4096, PAGE_SHIFT 12, ALIGNMENT 16, SELECT_ALL 0x2A, TAG_BITS 0x7FFFFFFF.
Status: Intact 0, CorruptBefore 1, CorruptAfter 2. Record: {ULONG Tag, Size, Offset, Flags}.
Fill byte: `(((pageAddr >> 12) * 37 + 0x5B) | 1)` truncated to u8 (per-page, odd/non-zero).

## Functions (C ABI `rust_pool_special_*`, mirroring the C)
- **Select(selector, bytes, tag) -> bool:** false if selector==0 || bytes==0 || bytes>=PAGE_SIZE;
  true if selector==0x2A; if selector<PAGE_SIZE -> size class (round bytes & selector up to 16,
  compare); else tag match over low 31 bits, per byte: `*` -> match, `?` -> skip, else must equal.
- **Place(bytes, alignment, at_start, &offset) -> bool:** offset=0; false if bytes==0||>=PAGE_SIZE;
  sanitise alignment (<16 || not pow2 || >PAGE_SIZE -> 16); aligned=roundup(bytes,alignment);
  end placement (default) -> offset = PAGE_SIZE - aligned; start -> 0.
- **Prepare(&record, page, bytes, offset, tag, flags) -> block:** fill the whole page with the
  pattern; record = {tag, bytes, offset, flags}; return page+offset.
- **Owns(&record, page, block) -> bool:** record.Size!=0 && block==page+record.Offset.
- **Check(&record, page, &corrupt) -> status:** corrupt=0; if offset>PAGE_SIZE ||
  size>PAGE_SIZE-offset -> corrupt=page, CorruptBefore; scan [0,offset) then [offset+size,PAGE_SIZE)
  for the pattern; first mismatch -> corrupt=&byte, CorruptBefore/After; else Intact.

By construction panic-free: wrapping/checked arithmetic, all page access bounded to PAGE_SIZE,
`from_raw_parts(_mut)` over the caller's page (the one `unsafe` surface, documented).

## Differential (`run-lrm-diff.sh`)
Links Dwight's `special.c` UNMODIFIED (compiled against a host `poolenv.h` stand-in) beside the
Rust staticlib and compares Select / Place / (Prepare+Owns+Check) over the reused `t_special.c`
cases + a deterministic fuzz, including **cross-engine** checks (C prepares, Rust checks, and vice
versa — the page-address pattern must agree) and the **exact corrupt address** on free. Latest:
**2,749,568 checks, 0 disagreements** (sel_true 150,860 / sel_false 350,097 / intact 999,001 /
corrupt 702,594). **Negative control:** perturbing the Rust fill pattern (0x5B->0x5C) -> 420,991
disagreements, so the harness is non-vacuous. Agreement on these inputs, not a proof of
equivalence. CI job `lrm-equivalence` runs it against the real `feat/special-pool` (pinned SHA).

## Orchestration (`src/mm.rs`) + full-stack differential (`run-lrm-fullstack.sh`)
`mm.rs` is the clean-room Rust of the glue in `ntspecial.c` (recovered from `feat/special-pool
3feb1c4f6a2`): `MmUseSpecialPool`, the full `MmAllocateSpecialPool`/`MmFreeSpecialPool`, and the
free-time 0xC1 classifier, over the `GuardRegion` FFI trait (an abstraction of Stanley's
`MiGuard*`, `nvs/sys/sysguard.c`). `GuardState` mirrors `MI_GUARD_KIND` exactly (Outside 0,
GuardPage 1, Live 2, Freed 3). Bug checks are returned as a `BugCheck` value (code + four params),
never a direct trap, so the kernel shim raises `KeBugCheckEx` and the host asserts.

- **Allocate** (mirrors `ntspecial.c`): null region -> NULL; `Irql > DISPATCH_LEVEL ||
  (paged && Irql > APC_LEVEL)` -> 0xC1 **(Irql, PoolType, Bytes, 0x30)**; `AtStart` from
  `SpecialType` (0 end / 1 start / else `MiSpecialPoolUnderruns`); alignment 64 iff
  `PoolType & CACHE_ALIGNED_POOL_MASK` else engine default; `executable` iff
  `ExpPoolHeapFromType(PoolType)==EXECUTABLE` (non-paged and NX bit clear) -> passed to the
  provider as a decoded bool; place/guard-allocate failure -> NULL; else `Prepare` block ptr.
- **Free** (mirrors `ntspecial.c` order): not-live-or-not-owned -> 0xC1 **(P,0,0,0x20)**;
  `Irql` too high for the block's pool type (`Record->Flags & base-type`) -> **(Irql,Flags,P,0x31)**;
  `Check` -> **(P,Corrupt,0,0x23/0x24)**; guard-free failure -> **(P,0,0,0x20)**; then refill.
- **POOL_TYPE decode:** `EX_POOL_BASE_TYPE_MASK=1`, `EX_POOL_HEAP_*`, `ExpPoolHeapFromType` from
  `expool.h`. `CACHE_ALIGNED_POOL_MASK=4` and `POOL_NX_ALLOCATION=0x200` are the public NT
  POOL_TYPE/WDM values (not in the private headers); the full-stack harness feeds the identical
  value to BOTH sides, so the equivalence is value-independent of those two constants.
- `SpecialType`/overruns follow the **C reference** (`ntspecial.c`), not Ryan's inverted doc row:
  `MmSpecialPoolCatchOverruns` default 1 = overruns = block at page END;
  `MiSpecialPoolUnderruns = (CatchOverruns==0)`.

`run-lrm-fullstack.sh` compiles `ntspecial.c` **UNMODIFIED** against a minimal faithful stub header
tree (`stub/`: `mint.h`/`expool.h` stand-ins + the real `poolspecial.h`/host `poolenv.h`) plus the
real `special.c` engine, over a single-slot mock `MiGuard` provider; `KeBugCheckEx` is captured via
`setjmp`/`longjmp`. The Rust side runs `lrm_mm_*` over the SAME mock (a C vtable `LrmGuardVtbl`).
For a randomized allocate/corrupt/free sequence across pool-type / special-type / IRQL / corruption
inputs and both overrun & underrun phases, it compares declined vs allocated (block offset) vs bug
check (code + sub-code + all four params, in-page addresses normalized to page offsets). Latest:
**3,405,895 checks, 0 disagreements**; coverage allocated 1,125,046 / declined 124,982 / bugchecks
0x20 179,433, 0x23 97,462, 0x24 293,039, 0x30 749,972, 0x31 354,064. **Negative control:**
perturbing `SpecialType` on the Rust side only (swap 0<->1) -> **155,338 disagreements**, so the
placement path is actually compared. It is chained from `run-lrm-diff.sh`, so the existing
`lrm-equivalence` CI job runs it; `NTSPECIAL_C` defaults to the vendored copy and CI may set it to
the SP_SHA-fetched `ntoskrnl/nvs/nt/ntspecial.c` for full pinning.

## Build note
Within `tools/rust-poc` the lnrspoc `.cargo/config.toml` targets the kernel, so the host build
stages the crate out (`run-lrm-diff.sh`). `special.c` is compiled unmodified by assembling
`include/{poolspecial.h,poolenv.h}` + `special/special.c` so its relative includes resolve to the
host `poolenv.h` stand-in. `difftest/dw/` vendors Dwight's recovered `special.c`+`poolspecial.h`
(provenance: feat/special-pool a42b006aab7) and `dw/nt/ntspecial.c` (provenance: feat/special-pool
3feb1c4f6a2, god-dropped) for the local run; CI uses the real files. The full-stack stub headers
(`stub/nvs/nt/mint.h`, `stub/nvs/pool/ex/expool.h`) are our minimal faithful stand-ins that
reproduce only what `ntspecial.c` uses (`ExpPoolHeapFromType` + the `EX_POOL_*` macros verbatim),
since the real `mint.h`/`expool.h` pull in the whole MM tree and cannot build on the host.
