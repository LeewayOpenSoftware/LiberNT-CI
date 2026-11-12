//! Leeway experimental Rust kernel-mode PoC (lnrspoc.sys backing staticlib).
//!
//! v1 (owner-approved 2026-11-12): no_std, no heap, no locks. A single C-ABI validator
//! that bounds-checks an untrusted length-prefixed record and returns its CRC-32.
//!
//! v2 (owner-approved 2026-11-12, `OWNER-OK-RUST-POC-V2`): adds a pool-backed
//! `#[global_allocator]` with fallible `try_*` APIs (OOM returns `Err`, never panics),
//! a `SpinLock<T>` RAII guard over `KSPIN_LOCK` (IRQL raised on acquire, restored on
//! drop), and bindgen-style NDK struct bindings with compile-time layout assertions
//! (acceptance test 4). The allocator and lock never bugcheck; the only bugcheck path
//! remains the gated, test-only `force_panic`.
//!
//! This crate is a `staticlib`; `KeBugCheckEx`, the `Ex*` pool routines and the `Ke*`
//! spin-lock routines stay unresolved here and bind to ntoskrnl when the driver links.
//! In the default (non-`test-ioctls`) build no panic is reachable, so LTO removes the
//! panic handler and its `KeBugCheckEx` reference entirely.
#![no_std]

extern crate alloc;

use core::ffi::{c_int, c_ulong};
use core::panic::PanicInfo;
use core::slice;

pub mod alloc_pool;
pub mod ndk;
pub mod sync;

use alloc_pool::KBox;
use sync::SpinLock;

/// Leeway provisional EXPERIMENTAL bugcheck code (0x4C450001, 'LE'+01) for an
/// unrecoverable Rust-side panic. Owner-approved 2026-11-12; exclusive to lnrspoc tests.
/// Documented in README.md "Bugcheck code registry". Not a Microsoft code; ours, for
/// our kernel. Only ever reached via the panic handler, never on normal init/op.
const LEEWAY_RUST_PANIC: c_ulong = 0x4C45_0001;

extern "C" {
    /// Supplied by ntoskrnl at driver link time. Never returns.
    fn KeBugCheckEx(code: c_ulong, p1: usize, p2: usize, p3: usize, p4: usize) -> !;
}

/// panic = abort => this never unwinds; it converts a Rust panic into a *controlled*
/// bugcheck. No allocation, no locks, no heap formatting: IRQL-agnostic and minimal.
#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    let (line, col) = match info.location() {
        Some(l) => (l.line() as usize, l.column() as usize),
        None => (0usize, 0usize),
    };
    // SAFETY: KeBugCheckEx is the standard NT bring-down-the-box routine; it never
    // returns and takes no locks we hold. Passing location in P1/P2 aids !analyze.
    unsafe { KeBugCheckEx(LEEWAY_RUST_PANIC, line, col, 0, 0) }
}

/// CRC-32 (IEEE 802.3, poly 0xEDB88320), table-free to keep the module tiny.
fn crc32_ieee(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &byte in data {
        crc ^= byte as u32;
        let mut i = 0;
        while i < 8 {
            let mask = (crc & 1).wrapping_neg(); // 0xFFFFFFFF if lsb set, else 0
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
            i += 1;
        }
    }
    !crc
}

// Negative return codes (success = 0). Mirrored in include/leeway_rs_poc.h.
const E_NULL: c_int = -1; // null buffer or out pointer
const E_SHORT: c_int = -2; // buffer too short for the 4-byte length prefix
const E_LENFIELD: c_int = -3; // declared record length exceeds available payload

/// Validate a length-prefixed record and compute the CRC-32 of its payload.
///
/// Layout: `[len: u32 little-endian][payload bytes...]`. `len` is the number of
/// payload bytes that follow the 4-byte prefix and must not exceed what `len_bytes`
/// makes available. On success writes the CRC-32 of exactly those `len` bytes to
/// `*out_crc` and returns 0; otherwise returns a negative code and leaves `*out_crc`
/// untouched. No allocation, no locking: safe to call at <= DISPATCH_LEVEL.
///
/// # Safety
/// `buf` must point to at least `len_bytes` readable bytes and `out_crc` to a writable
/// `c_ulong`, both valid for the call. Documented as the C-side contract in the header.
#[no_mangle]
pub unsafe extern "C" fn leeway_rs_poc_validate(
    buf: *const u8,
    len_bytes: c_ulong,
    out_crc: *mut c_ulong,
) -> c_int {
    if buf.is_null() || out_crc.is_null() {
        return E_NULL;
    }
    let total = len_bytes as usize;
    if total < 4 {
        return E_SHORT;
    }
    // SAFETY: caller guarantees `buf` is valid for `total` bytes (see header contract).
    let data = unsafe { slice::from_raw_parts(buf, total) };
    let declared = u32::from_le_bytes([data[0], data[1], data[2], data[3]]) as usize;
    let payload = &data[4..];
    if declared > payload.len() {
        return E_LENFIELD;
    }
    let crc = crc32_ieee(&payload[..declared]);
    // SAFETY: out_crc checked non-null above; caller guarantees it is writable.
    unsafe { *out_crc = crc as c_ulong };
    0
}

// ===== v2: allocator + spin-lock exercisers (always compiled; never bugcheck) =======
//
// Each returns 0 on success or a distinct negative code. They allocate from and lock
// over NT primitives, so they add the sanctioned Ex*/Ke* externs to every build, but
// none of them can panic, so the default build still carries no KeBugCheckEx reference.

// v2 self-test return codes.
const E_V2_ALLOC: c_int = -10; // KBox allocation failed
const E_V2_BOXVAL: c_int = -11; // value read back through KBox mismatched
const E_V2_LOCKVAL: c_int = -12; // value guarded by SpinLock mismatched
const E_V2_STRESS_ALLOC: c_int = -20; // alloc-stress: an allocation failed mid-churn
const E_V2_STRESS_VAL: c_int = -21; // alloc-stress: read-back mismatch
const E_V2_LOCK_COUNT: c_int = -30; // lock-stress: final count wrong
const E_V2_OOM_NONE: c_int = -40; // oom-probe: pool yielded nothing at all

/// Load-time self-test for v2: one pooled box and one spin-lock critical section.
/// Returns 0 on success. Safe at PASSIVE_LEVEL (acquire raises to DISPATCH and the
/// guard restores on drop).
#[no_mangle]
pub extern "C" fn leeway_rs_poc_v2_selftest() -> c_int {
    // Fallible pool allocation: OOM would return Err here, never trap.
    let b = match KBox::try_new(0x1234_5678_9abc_def0u64) {
        Ok(b) => b,
        Err(_) => return E_V2_ALLOC,
    };
    if *b != 0x1234_5678_9abc_def0u64 {
        return E_V2_BOXVAL;
    }
    drop(b);

    // Spin-lock critical section guarding a counter; IRQL restored when the guard drops.
    let guarded = SpinLock::new(0u64);
    guarded.init();
    {
        let mut g = guarded.lock();
        *g = g.wrapping_add(7);
    } // guard drops here: lock released, IRQL restored
    if *guarded.lock() != 7 {
        return E_V2_LOCKVAL;
    }
    0
}

/// Allocation stress: `iters` rounds of allocate, write, read-back, free. Exercises the
/// pool `#[global_allocator]` and `KBox` fallible path under churn. Returns 0, or a
/// negative code if an allocation failed or a value did not survive the round trip.
#[no_mangle]
pub extern "C" fn leeway_rs_poc_alloc_stress(iters: c_ulong) -> c_int {
    let n = iters as u64;
    let mut i: u64 = 0;
    while i < n {
        // Vary the payload so the write/read-back is meaningful and not optimized away.
        let token = i.wrapping_mul(0x9E37_79B9_7F4A_7C15).rotate_left(17);
        let b = match KBox::try_new(token) {
            Ok(b) => b,
            Err(_) => return E_V2_STRESS_ALLOC,
        };
        if *b != token {
            return E_V2_STRESS_VAL;
        }
        drop(b);
        i = i.wrapping_add(1);
    }
    0
}

/// Lock stress: `iters` acquire/modify/release cycles on one `SpinLock`, then verify the
/// guarded counter equals `iters`. Exercises repeated IRQL raise/restore and the RAII
/// release path. Single-threaded (one IOCTL thread), so the count is deterministic.
#[no_mangle]
pub extern "C" fn leeway_rs_poc_lock_stress(iters: c_ulong) -> c_int {
    let n = iters as u64;
    let counter = SpinLock::new(0u64);
    counter.init();
    let mut i: u64 = 0;
    while i < n {
        let mut g = counter.lock();
        *g = g.wrapping_add(1);
        // g drops here each iteration: release + IRQL restore, then re-acquire.
        i = i.wrapping_add(1);
    }
    if *counter.lock() != n {
        return E_V2_LOCK_COUNT;
    }
    0
}

/// OOM negative test: deliberately exhaust the nonpaged pool and prove the failure is a
/// returned `Err`, not a crash. Returns the number of 64 KiB blocks allocated before the
/// pool said no (clamped to a positive `c_int`), or `E_V2_OOM_NONE` if not even one
/// block could be taken. Reaching here at all is the proof: allocation failure did not
/// panic or bugcheck. All blocks are freed before return.
#[no_mangle]
pub extern "C" fn leeway_rs_poc_oom_probe() -> c_int {
    const BLOCK: usize = 64 * 1024;
    const CAP: usize = 1_000_000; // ~64 GiB ceiling; the pool fails long before this.
    let count = alloc_pool::oom_probe(BLOCK, CAP);
    if count == 0 {
        return E_V2_OOM_NONE;
    }
    if count > c_int::MAX as usize {
        return c_int::MAX;
    }
    count as c_int
}

/// TEST-ONLY controlled-bugcheck trigger. Compiled solely under the `test-ioctls`
/// feature and intended to be reachable only via an explicit, documented test IOCTL
/// in a disposable VM — never present in a normal build, never on init/normal op.
/// Proves the panic -> KeBugCheckEx bridge without unwinding across the FFI boundary.
#[cfg(feature = "test-ioctls")]
#[no_mangle]
pub extern "C" fn leeway_rs_poc_force_panic() -> ! {
    panic!("lnrspoc: explicit test-IOCTL panic (controlled bugcheck path)")
}
