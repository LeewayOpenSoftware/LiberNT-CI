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

## Build note
Within `tools/rust-poc` the lnrspoc `.cargo/config.toml` targets the kernel, so the host build
stages the crate out (`run-lrm-diff.sh`). `special.c` is compiled unmodified by assembling
`include/{poolspecial.h,poolenv.h}` + `special/special.c` so its relative includes resolve to the
host `poolenv.h` stand-in. `difftest/dw/` vendors Dwight's recovered `special.c`+`poolspecial.h`
(provenance: feat/special-pool a42b006aab7) for the local run; CI uses the real files.
