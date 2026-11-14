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
#![no_std]

use core::ffi::c_void;
use core::panic::PanicInfo;
use core::slice;

#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

pub mod mm;

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
    // SAFETY: caller guarantees `page` is a writable PAGE_SIZE page.
    let page_bytes = unsafe { slice::from_raw_parts_mut(p, PAGE_SIZE) };
    for b in page_bytes.iter_mut() {
        *b = fill;
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

    // SAFETY: page is readable for PAGE_SIZE.
    let bytes = unsafe { slice::from_raw_parts(p, PAGE_SIZE) };

    let mut i = 0usize;
    while i < offset {
        if bytes[i] != pat {
            if !corrupt.is_null() {
                unsafe { *corrupt = p.add(i) as usize };
            }
            return CORRUPT_BEFORE;
        }
        i += 1;
    }
    let mut j = offset + size;
    while j < PAGE_SIZE {
        if bytes[j] != pat {
            if !corrupt.is_null() {
                unsafe { *corrupt = p.add(j) as usize };
            }
            return CORRUPT_AFTER;
        }
        j += 1;
    }
    INTACT
}
