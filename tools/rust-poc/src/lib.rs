//! Leeway experimental Rust kernel-mode PoC (lnrspoc.sys backing staticlib), v1.
//!
//! Scope (owner-approved 2026-11-12): no_std, NO heap, NO locks. Exposes one C-ABI
//! function that validates an untrusted, length-prefixed byte record and returns its
//! CRC-32 (IEEE). Memory-safety is the point: every buffer access is bounds-checked.
//!
//! This crate is a `staticlib`; `KeBugCheckEx` stays unresolved here and binds to
//! ntoskrnl only when the driver links. No panic is reachable on normal operation
//! (every expected failure returns a negative status); panic is wired to a controlled
//! bugcheck for unrecoverable invariant violations only (see the panic handler).
#![no_std]

use core::ffi::{c_int, c_ulong};
use core::panic::PanicInfo;
use core::slice;

/// Leeway-reserved EXPERIMENTAL bugcheck code for an unrecoverable Rust-side panic.
/// Documented in README.md "Bugcheck code registry". Not a Microsoft code; ours, for
/// our kernel. Only ever reached via the panic handler, never on normal init/op.
const LEEWAY_RUST_PANIC: c_ulong = 0xE000_0001;

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

/// TEST-ONLY controlled-bugcheck trigger. Compiled solely under the `test-ioctls`
/// feature and intended to be reachable only via an explicit, documented test IOCTL
/// in a disposable VM — never present in a normal build, never on init/normal op.
/// Proves the panic -> KeBugCheckEx bridge without unwinding across the FFI boundary.
#[cfg(feature = "test-ioctls")]
#[no_mangle]
pub extern "C" fn leeway_rs_poc_force_panic() -> ! {
    panic!("lnrspoc: explicit test-IOCTL panic (controlled bugcheck path)")
}
