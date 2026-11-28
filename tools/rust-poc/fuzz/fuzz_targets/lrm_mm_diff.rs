#![no_main]
//! Differential libFuzzer target for the LRM-RS mm-orchestration free-time CLASSIFIER.
//!
//! `lrm_mm_free_classify` decides, on a special-pool free, whether to bug-check and with which
//! `SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION` (0xC1) sub-code and parameters. This target runs the
//! Rust classifier against an INDEPENDENT in-file oracle of the observed contract (the same spec
//! `lrm-rs/difftest/sp_diff.cpp`'s test_mm pins, generalised) for a fuzzer-chosen free state, and
//! asserts they agree on whether a bug check is raised and, if so, on code + all four parameters.
//! Any divergence or panic fails the run.
//!
//! Precedence (observed contract): not-live-or-not-owned → 0x20 (p1=p); else IRQL too high for the
//! block's pool type → 0x31 (p1=irql, p2=flags, p3=p); else fill corruption → 0x23/0x24
//! (p1=p, p2=corrupt); else the free proceeds.
//!
//! The classifier is called through the Rust path (`lrm_rs::mm::lrm_mm_free_classify`) so the
//! crate's object is linked; a bare `extern "C"` to the `#[no_mangle]` symbol does not pull it in.
//!
//! Run on the Linux private CI (libFuzzer); not built on the Windows host:
//!   cargo +nightly-2026-05-24 fuzz run lrm_mm_diff -- -max_total_time=120
//! Seed corpus: fuzz/corpus/lrm_mm_diff/.

use libfuzzer_sys::fuzz_target;
use lrm_rs::mm::{lrm_mm_free_classify, BugCheck};

const BUGCHECK_SPECIAL_POOL: u32 = 0xC1;
const SUB_NOT_ALLOCATED: usize = 0x20;
const SUB_CORRUPT_BEFORE: usize = 0x23;
const SUB_CORRUPT_AFTER: usize = 0x24;
const SUB_FREE_IRQL: usize = 0x31;

const GUARD_LIVE: i32 = 2; // GuardState::from_raw(2) == Live; everything else is not a live block
const APC_LEVEL: u8 = 1;
const DISPATCH_LEVEL: u8 = 2;

const POOL_CORRUPT_BEFORE: i32 = 1;
const POOL_CORRUPT_AFTER: i32 = 2;

fn sp(p1: usize, p2: usize, p3: usize, p4: usize) -> BugCheck {
    BugCheck { code: BUGCHECK_SPECIAL_POOL, p1, p2, p3, p4 }
}

/// Independent oracle of the free-time classifier contract. `None` = the free proceeds.
#[allow(clippy::too_many_arguments)]
fn oracle(
    p: usize,
    query: i32,
    owns: bool,
    irql: u8,
    paged: bool,
    flags: usize,
    check_status: i32,
    corrupt: usize,
) -> Option<BugCheck> {
    if query != GUARD_LIVE || !owns {
        return Some(sp(p, 0, 0, SUB_NOT_ALLOCATED));
    }
    if irql > DISPATCH_LEVEL || (paged && irql > APC_LEVEL) {
        return Some(sp(irql as usize, flags, p, SUB_FREE_IRQL));
    }
    match check_status {
        POOL_CORRUPT_BEFORE => Some(sp(p, corrupt, 0, SUB_CORRUPT_BEFORE)),
        POOL_CORRUPT_AFTER => Some(sp(p, corrupt, 0, SUB_CORRUPT_AFTER)),
        _ => None,
    }
}

struct Cur<'a> {
    d: &'a [u8],
    i: usize,
}
impl<'a> Cur<'a> {
    fn new(d: &'a [u8]) -> Self {
        Cur { d, i: 0 }
    }
    fn u8(&mut self) -> u8 {
        let v = self.d.get(self.i).copied().unwrap_or(0);
        self.i += 1;
        v
    }
    fn usize(&mut self) -> usize {
        let mut v = 0usize;
        for _ in 0..8 {
            v = (v << 8) | self.u8() as usize;
        }
        v
    }
    fn small(&mut self) -> i32 {
        // Raw guard/status values in a range that covers all branches plus junk.
        (self.u8() % 6) as i32
    }
}

fuzz_target!(|data: &[u8]| {
    let mut c = Cur::new(data);
    let p = c.usize();
    let query = c.small(); // 0..5: Outside/GuardPage/Live/Freed + junk
    let owns = c.u8() & 1;
    let irql = c.u8() % 4; // PASSIVE..HIGH, exercises the > DISPATCH / paged>APC branches
    let paged = c.u8() & 1;
    let flags = c.usize();
    let check_status = c.small(); // 0..5: Intact/Before/After + junk
    let corrupt = c.usize();

    let mut got = BugCheck::default();
    let raised =
        unsafe { lrm_mm_free_classify(p, query, owns, irql, paged, flags, check_status, corrupt, &mut got) };

    let want = oracle(p, query, owns != 0, irql, paged != 0, flags, check_status, corrupt);
    match want {
        None => assert_eq!(raised, 0, "oracle says proceed but classifier raised: {got:?}"),
        Some(expected) => {
            assert_eq!(raised, 1, "oracle says bug check {expected:?} but classifier proceeded");
            assert_eq!(got, expected, "bug-check mismatch");
        }
    }
});
