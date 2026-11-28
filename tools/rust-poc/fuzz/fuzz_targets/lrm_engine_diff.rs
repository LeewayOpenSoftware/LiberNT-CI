#![no_main]
//! Differential libFuzzer target for the LRM-RS special-pool ENGINE (R-? fuzz task).
//!
//! Drives the SAME NT contract on both engines for a fuzzer-chosen operation and asserts they
//! agree: Dwight's C engine (`special.c`, compiled by build.rs as the oracle) vs the Rust engine
//! (`lrm_rs::rust_pool_special_*`). Covers select / place / (prepare + owns + check, clean and
//! one-byte-corrupted). Any divergence — return value, written offset, block pointer, record
//! bytes, check status, or the EXACT corrupt address — fails the run; so does any panic. This is
//! the coverage-guided counterpart to the fixed-RNG `lrm-rs/difftest/sp_diff.cpp`.
//!
//! Run on the Linux private CI (libFuzzer); not built on the Windows host:
//!   cargo +nightly-2026-05-24 fuzz run lrm_engine_diff -- -max_total_time=300
//! Seed corpus: fuzz/corpus/lrm_engine_diff/.

use core::ffi::c_void;
use libfuzzer_sys::fuzz_target;
use lrm_rs::PoolSpecialRecord;

const PAGE_SIZE: usize = 4096;

// C reference engine (special.c), same POOL_SPECIAL_RECORD layout as lrm_rs::PoolSpecialRecord.
// BOOLEAN is an unsigned char in NT, so the C booleans come back as u8 and are normalised to 0/1
// before comparison (the C engine may return any non-zero for true).
extern "C" {
    fn PoolSpecialSelect(selector: u32, bytes: usize, tag: u32) -> u8;
    fn PoolSpecialPlace(bytes: usize, alignment: u32, at_start: u8, offset: *mut u32) -> u8;
    fn PoolSpecialPrepare(
        record: *mut PoolSpecialRecord,
        page: *mut c_void,
        bytes: usize,
        offset: u32,
        tag: u32,
        flags: u32,
    ) -> *mut c_void;
    fn PoolSpecialOwns(record: *const PoolSpecialRecord, page: *mut c_void, block: *mut c_void) -> u8;
    fn PoolSpecialCheck(record: *const PoolSpecialRecord, page: *mut c_void, corrupt: *mut usize) -> i32;
}

// Rust engine (same contract, distinct symbols), from the lrm-rs crate built with the `std` feature.
use lrm_rs::{
    rust_pool_special_check, rust_pool_special_owns, rust_pool_special_place,
    rust_pool_special_prepare, rust_pool_special_select,
};

/// Little-endian byte cursor; missing bytes read as 0, so short inputs are still valid operations.
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
    fn u32(&mut self) -> u32 {
        let mut b = [0u8; 4];
        for k in 0..4 {
            b[k] = self.u8();
        }
        u32::from_le_bytes(b)
    }
}

#[inline]
fn norm(b: u8) -> u8 {
    (b != 0) as u8
}

fn do_select(c: &mut Cur) {
    let selector = c.u32();
    let bytes = (c.u32() as usize) % (PAGE_SIZE + 2);
    let tag = c.u32();
    let cc = norm(unsafe { PoolSpecialSelect(selector, bytes, tag) });
    let rr = norm(rust_pool_special_select(selector, bytes, tag));
    assert_eq!(cc, rr, "select selector=0x{selector:08x} bytes={bytes} tag=0x{tag:08x}");
}

fn do_place(c: &mut Cur) {
    let bytes = (c.u32() as usize) % (PAGE_SIZE + 2);
    let alignment = c.u32();
    let at_start = c.u8() & 1;
    let mut oc: u32 = 0xDEAD_BEEF;
    let mut or_: u32 = 0x0BAD_F00D;
    let rc = norm(unsafe { PoolSpecialPlace(bytes, alignment, at_start, &mut oc) });
    let rr = norm(unsafe { rust_pool_special_place(bytes, alignment, at_start, &mut or_) });
    assert_eq!(rc, rr, "place.ret bytes={bytes} align=0x{alignment:08x} at_start={at_start}");
    assert_eq!(oc, or_, "place.offset bytes={bytes} align=0x{alignment:08x} at_start={at_start}");
}

fn do_prepare_check(c: &mut Cur) {
    // A valid in-range placement, as sp_diff does: size in 1..=512, offset from the engine's place.
    let bytes = 1 + (c.u32() as usize) % 512;
    let alignment = 16u32 << (c.u8() % 5); // 16,32,64,128,256
    let at_start = c.u8() & 1;
    let tag = c.u32();
    let flags = c.u32();
    // -1 means "no corruption"; else a byte position in the page.
    let corrupt_sel = c.u8();
    let corrupt_pos: isize = if corrupt_sel & 3 == 0 {
        -1
    } else {
        (c.u32() as usize % PAGE_SIZE) as isize
    };

    let mut offset: u32 = 0;
    let _ = unsafe { rust_pool_special_place(bytes, alignment, at_start, &mut offset) };

    // A page aligned to PAGE_SIZE, from a 3-page backing store (both engines refill it identically).
    let mut backing = vec![0u8; PAGE_SIZE * 3];
    let base = backing.as_mut_ptr() as usize;
    let page = ((base + PAGE_SIZE - 1) & !(PAGE_SIZE - 1)) as *mut u8;

    let mut rec_c = PoolSpecialRecord { tag: 0, size: 0, offset: 0, flags: 0 };
    let mut rec_r = PoolSpecialRecord { tag: 0, size: 0, offset: 0, flags: 0 };

    let blk_c = unsafe {
        PoolSpecialPrepare(&mut rec_c, page as *mut c_void, bytes, offset, tag, flags)
    };
    let blk_r = unsafe {
        rust_pool_special_prepare(&mut rec_r, page as *mut c_void, bytes, offset, tag, flags)
    };
    assert_eq!(blk_c, blk_r, "prepare.block bytes={bytes} offset={offset}");
    assert!(
        rec_c.tag == rec_r.tag
            && rec_c.size == rec_r.size
            && rec_c.offset == rec_r.offset
            && rec_c.flags == rec_r.flags,
        "prepare.record c=({},{},{},{}) rust=({},{},{},{})",
        rec_c.tag, rec_c.size, rec_c.offset, rec_c.flags,
        rec_r.tag, rec_r.size, rec_r.offset, rec_r.flags
    );

    let oc = norm(unsafe { PoolSpecialOwns(&rec_c, page as *mut c_void, blk_c) });
    let or_ = norm(unsafe { rust_pool_special_owns(&rec_r, page as *mut c_void, blk_r) });
    assert_eq!(oc, or_, "owns bytes={bytes} offset={offset}");

    // Check on the clean page: status and the (zeroed) corrupt address must agree.
    let mut cc: usize = 1;
    let mut cr: usize = 1;
    let sc = unsafe { PoolSpecialCheck(&rec_c, page as *mut c_void, &mut cc) };
    let sr = unsafe { rust_pool_special_check(&rec_r, page as *mut c_void, &mut cr) };
    assert_eq!(sc, sr, "check.clean.status bytes={bytes} offset={offset}");
    assert_eq!(cc, cr, "check.clean.addr bytes={bytes} offset={offset}");

    if corrupt_pos >= 0 && (corrupt_pos as usize) < PAGE_SIZE {
        let p = corrupt_pos as usize;
        let saved = unsafe { *page.add(p) };
        unsafe { *page.add(p) = saved ^ 0x80 };
        let mut dc: usize = 0;
        let mut dr: usize = 0;
        let tc = unsafe { PoolSpecialCheck(&rec_c, page as *mut c_void, &mut dc) };
        let tr = unsafe { rust_pool_special_check(&rec_r, page as *mut c_void, &mut dr) };
        assert_eq!(tc, tr, "check.corrupt.status pos={p} bytes={bytes} offset={offset}");
        assert_eq!(dc, dr, "check.corrupt.addr pos={p} bytes={bytes} offset={offset}");
        unsafe { *page.add(p) = saved };
    }
}

fuzz_target!(|data: &[u8]| {
    let mut c = Cur::new(data);
    match c.u8() % 3 {
        0 => do_select(&mut c),
        1 => do_place(&mut c),
        _ => do_prepare_check(&mut c),
    }
});
