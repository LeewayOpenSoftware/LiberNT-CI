//! R1c — experimental Rust port of NtfsDecodeMappingPairs, behind the ntfslib host-test
//! rig's C ABI (`ntfs_decode_impl` / `ntfs_alt_impl`, decode_diff.h).
//!
//! Pure, `no_std`, alloc-free, lock-free, panic-free decoder of an NTFS nonresident
//! attribute's mapping pairs (the run list). It mirrors the shipped C `NtfsDecodeMappingPairs`
//! — which is the rig's reference — and the independent `model.c`; the rig (`ntfs_decode_diff
//! _test`) feeds the same untrusted bytes to lib / model / this alt impl and fails on any
//! disagreement. Removes the mapping-pairs defect class (signed variable-width fields, two
//! overflow directions, volume-bound checks, untrusted terminator) by construction: every
//! read is bounds-checked (`rd_le` returns `Option`), every sum is `checked_add`, and the
//! sign handling is explicit. One `unsafe` (`from_raw_parts`) over the caller's attribute.
//!
//! Invariants M1–M11 are in `sdk/lib/fslib/ntfslib_new/linux/tests/decode_diff/decode_diff.md`
//! and restated in `difftest/MAPPING-PAIRS-INVARIANTS.md`.
#![no_std]

use core::ffi::c_void;
use core::panic::PanicInfo;
use core::slice;

#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    // Unreachable: the decoder is panic-free by construction (M11). Spin if ever hit.
    loop {
        core::hint::spin_loop();
    }
}

// NTSTATUS-style results used by the rig (decode_diff.h).
const NTFS_DIFF_SUCCESS: i32 = 0x0000_0000u32 as i32;
const NTFS_DIFF_INVALID_PARAMETER: i32 = 0xC000_000Du32 as i32;
const NTFS_DIFF_FILE_CORRUPT: i32 = 0xC000_0102u32 as i32;

/// `int32_t (*)(void *ctx, uint64_t lcn, uint64_t length, uint8_t is_sparse)`.
type RunSink = Option<unsafe extern "C" fn(*mut c_void, u64, u64, u8) -> i32>;

/// Mirror of `struct ntfs_decode_impl` (decode_diff.h). Field order and ABI must match.
#[repr(C)]
pub struct NtfsDecodeImpl {
    name: *const u8,
    decode_mapping_pairs:
        Option<unsafe extern "C" fn(*const u8, u32, u64, RunSink, *mut c_void) -> i32>,
    apply_fixup: Option<unsafe extern "C" fn(*mut u8, u32, u32) -> i32>,
    index_entry_valid: Option<unsafe extern "C" fn(*const u8, u32) -> i32>,
}

// A table is immutable and shared read-only; the raw pointer field is never written.
struct Table(NtfsDecodeImpl);
unsafe impl Sync for Table {}

static ALT: Table = Table(NtfsDecodeImpl {
    name: b"rust\0".as_ptr(),
    decode_mapping_pairs: Some(decode_mapping_pairs),
    apply_fixup: None,       // not implemented (skipped by the rig)
    index_entry_valid: None, // not implemented (skipped by the rig)
});

/// Rig entry point: `const ntfs_decode_impl *ntfs_alt_impl(void)`.
#[no_mangle]
pub extern "C" fn ntfs_alt_impl() -> *const NtfsDecodeImpl {
    &ALT.0
}

// ---- bounded little-endian read of 1..8 bytes (never panics / reads OOB) ----
#[inline]
fn rd_le(buf: &[u8], off: u32, n: u32) -> Option<u64> {
    let o = off as usize;
    let end = o.checked_add(n as usize)?;
    let b = buf.get(o..end)?;
    let mut v: u64 = 0;
    let mut i = 0usize;
    while i < b.len() {
        v |= (b[i] as u64) << (8 * i);
        i += 1;
    }
    Some(v)
}

/// True if the `n`-byte little-endian value `v` has its sign bit set.
#[inline]
fn is_negative(v: u64, n: u32) -> bool {
    n != 0 && ((v >> (8 * n - 1)) & 1) != 0
}

/// Core decoder over a borrowed attribute slice. Mirrors `ModelMappingPairs` /
/// `NtfsDecodeMappingPairs` exactly; see M1–M11.
fn decode(a: &[u8], clusters: u64, sink: RunSink, ctx: *mut c_void) -> i32 {
    // M1: nothing is read when avail < 0x40 (then corrupt).
    if a.len() < 0x40 {
        return NTFS_DIFF_FILE_CORRUPT;
    }
    // M2: nonresident byte (0x08) must be set, else INVALID_PARAMETER and nothing else read.
    if a[8] == 0 {
        return NTFS_DIFF_INVALID_PARAMETER;
    }
    let length = match rd_le(a, 4, 4) {
        Some(v) => v as u32,
        None => return NTFS_DIFF_FILE_CORRUPT,
    };
    let flags = match rd_le(a, 0x0C, 2) {
        Some(v) => v as u32,
        None => return NTFS_DIFF_FILE_CORRUPT,
    };
    // M3: header is 0x48 when compressed/sparse (Flags & 0x80FF), else 0x40.
    let header: u32 = if (flags & 0x80FF) != 0 { 0x48 } else { 0x40 };
    let offset = match rd_le(a, 0x20, 2) {
        Some(v) => v as u32,
        None => return NTFS_DIFF_FILE_CORRUPT,
    };
    let avail = a.len() as u32;
    if length > avail || length < header || offset < header || offset >= length {
        return NTFS_DIFF_FILE_CORRUPT;
    }

    let mut previous: u64 = 0;
    let mut total: u64 = 0;
    let mut pos: u32 = offset;

    // M8: a 0x00 byte inside [DataRunsOffset, Length) ends the list.
    while pos < length {
        let head = match a.get(pos as usize) {
            Some(&b) => b,
            None => return NTFS_DIFF_FILE_CORRUPT,
        };
        if head == 0 {
            break;
        }
        let ls = (head & 0x0F) as u32;
        let os = (head >> 4) as u32;
        // M4: 1 <= L <= 8, O <= 8, and 1+L+O bytes before Length.
        if ls == 0 || ls > 8 || os > 8 || length - pos < 1 + ls + os {
            return NTFS_DIFF_FILE_CORRUPT;
        }
        // M5: run length is the L-byte signed LE value and must be > 0.
        let run = match rd_le(a, pos + 1, ls) {
            Some(v) => v,
            None => return NTFS_DIFF_FILE_CORRUPT,
        };
        // M6: the sum of run lengths never wraps in 64 bits.
        if run == 0 || is_negative(run, ls) || total.checked_add(run).is_none() {
            return NTFS_DIFF_FILE_CORRUPT;
        }

        let mut lcn: u64 = 0;
        if os != 0 {
            // M7: LCN = previous + signed delta; sparse only when O == 0.
            let raw = match rd_le(a, pos + 1 + ls, os) {
                Some(v) => v,
                None => return NTFS_DIFF_FILE_CORRUPT,
            };
            if is_negative(raw, os) {
                let ext = if os == 8 { raw } else { (!0u64 << (8 * os)) | raw };
                let back = (!ext).wrapping_add(1); // magnitude of the negative delta
                if previous < back {
                    return NTFS_DIFF_FILE_CORRUPT; // LCN would go below 0
                }
                lcn = previous - back;
            } else {
                match previous.checked_add(raw) {
                    Some(v) => lcn = v,
                    None => return NTFS_DIFF_FILE_CORRUPT, // LCN would wrap above 2^64-1
                }
            }
            // M7: LCN < clusters and length <= clusters - LCN.
            if lcn >= clusters || run > clusters - lcn {
                return NTFS_DIFF_FILE_CORRUPT;
            }
            previous = lcn;
        }

        // M10: runs reach the sink in on-disk order, before the final checks; a sink
        // failure stops the decode and is returned unchanged.
        if let Some(f) = sink {
            let st = unsafe { f(ctx, lcn, run, if os == 0 { 1 } else { 0 }) };
            if st < 0 {
                return st;
            }
        }

        total = total.wrapping_add(run); // no wrap: checked above
        pos += 1 + ls + os;
    }

    // M8: without a terminator inside [offset, length) the attribute is corrupt.
    if pos >= length {
        return NTFS_DIFF_FILE_CORRUPT;
    }

    // M9: LastVCN >= FirstVCN, and sum of lengths == LastVCN - FirstVCN + 1 (mod 2^64),
    // so the empty list with FirstVCN=0, LastVCN=-1 is valid.
    let first = match rd_le(a, 0x10, 8) {
        Some(v) => v,
        None => return NTFS_DIFF_FILE_CORRUPT,
    };
    let last = match rd_le(a, 0x18, 8) {
        Some(v) => v,
        None => return NTFS_DIFF_FILE_CORRUPT,
    };
    if last < first || total != last.wrapping_sub(first).wrapping_add(1) {
        return NTFS_DIFF_FILE_CORRUPT;
    }

    NTFS_DIFF_SUCCESS
}

/// C ABI: decode the mapping pairs of the nonresident attribute at `attr`, reading at most
/// `avail` bytes, reporting each run to `sink` in order. See decode_diff.h / decode_diff.md.
///
/// # Safety
/// `attr` must be NULL or point to `avail` readable bytes valid for the call; `sink` (if
/// non-NULL) must be a valid C function. The buffer is borrowed, never retained.
///
/// No `#[no_mangle]`: this is reached only through the `ntfs_alt_impl()` table's function
/// pointer, so it needs no exported C symbol (and must not clash with one).
unsafe extern "C" fn decode_mapping_pairs(
    attr: *const u8,
    avail: u32,
    clusters_in_volume: u64,
    sink: RunSink,
    context: *mut c_void,
) -> i32 {
    if attr.is_null() || avail < 0x40 {
        return NTFS_DIFF_FILE_CORRUPT; // M1
    }
    // SAFETY: caller guarantees `attr` is readable for `avail` bytes (decode_diff.h).
    let a = unsafe { slice::from_raw_parts(attr, avail as usize) };
    decode(a, clusters_in_volume, sink, context)
}
