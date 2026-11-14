//! LRM-RS — Leeway Rust Memory, Special Pool engine (R-1 lead task).
//!
//! A clean-room Rust implementation of the SAME NT special-pool ENGINE contract as Dwight's C
//! (`ntoskrnl/nvs/pool/special/special.c` + `poolspecial.h`, `feat/special-pool a42b006aab7`):
//! selection by size-class / tag (with `?`/`*` wildcards / `0x2A` select-all), placement at the
//! page end or start with 16/64/pow2 alignment, a page-address-derived fill pattern written on
//! prepare and verified on free with the exact corrupt address, and a small side record.
//!
//! OUR code — behaviour/contracts observed from ARM3/NT may be read, never copied (owner
//! decision 2026-11-14, hybrid). The C engine stays the differential reference and the in-kernel
//! fallback; a production swap needs the owner's OK.
//!
//! `no_std`, no heap, lock-free, panic-free by construction (all arithmetic is wrapping/checked,
//! all page access is bounds-limited to `PAGE_SIZE`). Exposed over a C ABI (`rust_pool_special_*`)
//! so a host harness can link it beside the C engine and diff the two. The caller owns the page
//! and the record; this engine reads/writes only the given page bytes and the record.
#![cfg_attr(not(feature = "std"), no_std)]

use core::ffi::c_void;

// The kernel staticlib is no_std and owns its panic handler; the host `std` build (differential and
// fuzz harnesses) links against std's, so the handler + its FFI are compiled out under `std`.
//
// Kernel panic policy (LRM-RS-PANIC-POLICY.md, owner-approved 2026-11-14): an unexpected Rust panic
// — which the panic-free discipline and the FORBID_PANIC build gate are meant to make unreachable —
// is turned into one controlled kernel bug check (LEEWAY_RUST_PANIC = 0x4C524D00 | reason), never a
// hang, a recovery, or undefined behaviour. panic = "abort" (Cargo.toml) means no unwinding can
// cross the `extern "C"` boundary. The handler does only field reads on `PanicInfo` and one
// non-returning FFI call: no heap, no `core::fmt`, no allocation. It is safe at any IRQL (including
// HIGH_LEVEL): the only memory it touches is the panic location's file string, which is a
// `&'static str` in the image's read-only data (never pageable). The numeric bug-check code and
// parameter assembly live in the kernel shim (ntoskrnl/nvs/nt/lrmrs.c), keeping one auditable site.
#[cfg(not(feature = "std"))]
extern "C" {
    /// Provided by the kernel shim (lrmrs.c) when `lrm_rs` is linked into ntoskrnl. Sets the global
    /// "Rust has stopped" flag and raises `KeBugCheckEx(LEEWAY_RUST_PANIC=0x4C524D00, reason, file,
    /// line, file_len)` — the code is FIXED (reason is in P1, never ORed into the code); never
    /// returns. Named without the substring "panic" so neither it nor a retained handler can mask or
    /// trip the FORBID_PANIC relocation gate (which greps for `panic`).
    fn LrmRsFatalStop(reason: u32, file: *const u8, file_len: usize, line: u32) -> !;
}

#[cfg(not(feature = "std"))]
#[panic_handler]
fn on_panic(info: &core::panic::PanicInfo) -> ! {
    let (file, file_len, line) = match info.location() {
        Some(loc) => (loc.file().as_ptr(), loc.file().len(), loc.line()),
        None => (core::ptr::null(), 0usize, 0u32),
    };
    // SAFETY: the kernel shim defines LrmRsFatalStop for every build that links lrm_rs; it never
    // returns. Reason 0x01 = a real Rust panic got through to the handler.
    unsafe { LrmRsFatalStop(0x01, file, file_len, line) }
}

/// Deliberate fatal stop chosen by Rust code (reason 0x02) — a contract/invariant the crate decides
/// must stop the machine. It forwards straight to `LrmRsFatalStop` and MUST NOT use `panic!` /
/// `unreachable!` / `assert!`: those are `core::panicking` call sites that the FORBID_PANIC gate
/// rejects (the same class as a slice-index panic). Reserved for future use; nothing invokes it yet.
#[cfg(not(feature = "std"))]
#[allow(unused_macros)]
macro_rules! lrm_fatal {
    () => {
        // SAFETY: LrmRsFatalStop is the kernel shim; never returns.
        unsafe { $crate::LrmRsFatalStop(0x02, ::core::file!().as_ptr(), ::core::file!().len(), ::core::line!()) }
    };
}

/// Deliberately panics, to validate the kernel panic policy end to end (this handler → the
/// `LEEWAY_RUST_PANIC` bug check) in a disposable VM on SMP1/SMP2. Feature-gated: absent from the
/// shipped FORBID_PANIC archive (so it stays 0 panic relocations) and from shipping ISOs; the lab
/// kernel enables the `panic-test` feature (FORBID_PANIC off) and drives it through an lnrspoc IOCTL.
#[cfg(all(not(feature = "std"), feature = "panic-test"))]
#[no_mangle]
pub extern "C" fn lrm_rs_force_panic() -> ! {
    panic!("LRM-RS forced panic self-test")
}

pub mod mm;
pub mod vf;

const PAGE_SIZE: usize = 4096;
const PAGE_SHIFT: u32 = 12;
const ALIGNMENT: u32 = 16; // POOL_SPECIAL_ALIGNMENT
const SELECT_ALL: u32 = 0x2A; // POOL_SPECIAL_SELECT_ALL
const TAG_BITS: u32 = 0x7FFF_FFFF; // POOL_SPECIAL_TAG_BITS

// POOL_SPECIAL_STATUS
const INTACT: i32 = 0;
const CORRUPT_BEFORE: i32 = 1;
const CORRUPT_AFTER: i32 = 2;

/// Mirror of `POOL_SPECIAL_RECORD` (poolspecial.h): four ULONGs.
#[repr(C)]
pub struct PoolSpecialRecord {
    pub tag: u32,
    pub size: u32,
    pub offset: u32,
    pub flags: u32,
}

// Mirrored by the C side's `C_ASSERT(sizeof(POOL_SPECIAL_RECORD) == 16)` (lrmrs.c). The MiGuard
// region is created with ContextBytes = sizeof(POOL_SPECIAL_RECORD); a size mismatch would corrupt
// the per-block record. Compile-time.
const _: () = assert!(core::mem::size_of::<PoolSpecialRecord>() == 16);

/// Page-address-derived fill byte, identical to `PoolSpecialPattern`:
/// `(((addr >> PAGE_SHIFT) * 37 + 0x5B) | 1)` truncated to a byte (always odd/non-zero).
#[inline]
fn pattern(page: *const u8) -> u8 {
    let addr = page as usize;
    ((((addr >> PAGE_SHIFT) as u64)
        .wrapping_mul(37)
        .wrapping_add(0x5B))
        | 1) as u8
}

#[inline]
fn round_up(x: usize, align: usize) -> usize {
    // align is a power of two; mirrors (x + align - 1) & ~(align - 1) with wrapping add.
    x.wrapping_add(align - 1) & !(align - 1)
}

/// `PoolSpecialSelect`: is a `Bytes`/`Tag` allocation selected for the special pool by `Selector`?
#[no_mangle]
pub extern "C" fn rust_pool_special_select(selector: u32, bytes: usize, tag: u32) -> u8 {
    if selector == 0 || bytes == 0 || bytes >= PAGE_SIZE {
        return 0;
    }
    if selector == SELECT_ALL {
        return 1;
    }
    if (selector as usize) < PAGE_SIZE {
        // size class: round both up to ALIGNMENT and compare.
        let a = ALIGNMENT as usize;
        return (round_up(bytes, a) == round_up(selector as usize, a)) as u8;
    }
    // tag match with ?/* wildcards, over the low 31 bits.
    let sel = selector & TAG_BITS;
    let t = tag & TAG_BITS;
    let mut pos = 0u32;
    while pos < 4 {
        let wanted = (sel >> (pos * 8)) as u8;
        let actual = (t >> (pos * 8)) as u8;
        if wanted == b'*' {
            return 1;
        }
        if wanted != b'?' && wanted != actual {
            return 0;
        }
        pos += 1;
    }
    1
}

/// `PoolSpecialPlace`: compute the in-page offset for a block of `Bytes`, aligned, at the page
/// end (default) or start. Writes `*offset` and returns 1 on success.
///
/// # Safety
/// `offset` must be a valid writable `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn rust_pool_special_place(
    bytes: usize,
    alignment: u32,
    at_start: u8,
    offset: *mut u32,
) -> u8 {
    if !offset.is_null() {
        // SAFETY: caller guarantees a writable u32.
        unsafe { *offset = 0 };
    }
    if bytes == 0 || bytes >= PAGE_SIZE {
        return 0;
    }
    let mut align = alignment;
    if align < ALIGNMENT || (align & (align - 1)) != 0 || align as usize > PAGE_SIZE {
        align = ALIGNMENT;
    }
    let aligned = round_up(bytes, align as usize);
    if at_start == 0 && !offset.is_null() {
        // SAFETY: as above.
        unsafe { *offset = (PAGE_SIZE - aligned) as u32 };
    }
    1
}

/// `PoolSpecialPrepare`: fill the whole page with the pattern, populate the record, and return
/// the block pointer (`page + offset`).
///
/// # Safety
/// `record` is a writable `PoolSpecialRecord`; `page` points to a writable `PAGE_SIZE` page.
#[no_mangle]
pub unsafe extern "C" fn rust_pool_special_prepare(
    record: *mut PoolSpecialRecord,
    page: *mut c_void,
    bytes: usize,
    offset: u32,
    tag: u32,
    flags: u32,
) -> *mut c_void {
    let p = page as *mut u8;
    let fill = pattern(p);
    // SAFETY: caller guarantees `page` is a writable PAGE_SIZE page. Write through the raw pointer
    // (no &mut slice over caller memory); volatile byte stores keep it a loop, never a `memset` call
    // (which the kernel staticlib's extern gate forbids).
    let mut k = 0usize;
    while k < PAGE_SIZE {
        unsafe { p.add(k).write_volatile(fill) };
        k += 1;
    }
    // SAFETY: caller guarantees `record` is writable.
    unsafe {
        (*record).tag = tag;
        (*record).size = bytes as u32;
        (*record).offset = offset;
        (*record).flags = flags;
    }
    unsafe { p.add(offset as usize) as *mut c_void }
}

/// `PoolSpecialOwns`: does `block` belong to this record's page/offset?
///
/// # Safety
/// `record` is a readable `PoolSpecialRecord`.
#[no_mangle]
pub unsafe extern "C" fn rust_pool_special_owns(
    record: *const PoolSpecialRecord,
    page: *mut c_void,
    block: *mut c_void,
) -> u8 {
    // SAFETY: caller guarantees `record` is readable.
    let (size, offset) = unsafe { ((*record).size, (*record).offset) };
    let expect = (page as *mut u8 as usize).wrapping_add(offset as usize);
    ((size != 0) && (block as usize == expect)) as u8
}

/// `PoolSpecialCheck`: verify the fill pattern around the block; on corruption, write the exact
/// corrupt address to `*corrupt` and return before/after; else Intact.
///
/// # Safety
/// `record` is readable; `page` points to a readable `PAGE_SIZE` page; `corrupt` is writable.
#[no_mangle]
pub unsafe extern "C" fn rust_pool_special_check(
    record: *const PoolSpecialRecord,
    page: *mut c_void,
    corrupt: *mut usize,
) -> i32 {
    let p = page as *const u8;
    let pat = pattern(p);
    if !corrupt.is_null() {
        // SAFETY: caller guarantees writable.
        unsafe { *corrupt = 0 };
    }
    // SAFETY: caller guarantees `record` readable.
    let (offset, size) = unsafe { ((*record).offset as usize, (*record).size as usize) };

    // Degenerate record: offset past the page, or size past the tail (checked subtraction).
    if offset > PAGE_SIZE || size > PAGE_SIZE.wrapping_sub(offset) {
        if !corrupt.is_null() {
            unsafe { *corrupt = page as usize };
        }
        return CORRUPT_BEFORE;
    }

    // SAFETY: page is readable for PAGE_SIZE. Read through the raw pointer with volatile loads - no
    // shared &[u8] over a page another CPU may be writing concurrently (that race is exactly the
    // corruption being detected, and a shared slice over it would be UB).
    let mut i = 0usize;
    while i < offset {
        if unsafe { p.add(i).read_volatile() } != pat {
            if !corrupt.is_null() {
                unsafe { *corrupt = p.add(i) as usize };
            }
            return CORRUPT_BEFORE;
        }
        i += 1;
    }
    let mut j = offset + size;
    while j < PAGE_SIZE {
        if unsafe { p.add(j).read_volatile() } != pat {
            if !corrupt.is_null() {
                unsafe { *corrupt = p.add(j) as usize };
            }
            return CORRUPT_AFTER;
        }
        j += 1;
    }
    INTACT
}
