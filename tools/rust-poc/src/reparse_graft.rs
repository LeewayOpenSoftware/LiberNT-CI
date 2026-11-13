//! R1b — IO-3 name-grafting reparse validator (experimental Rust, behind a C ABI).
//!
//! This is the first *real* lnrspoc payload: a pure, `no_std`, alloc-free, lock-free
//! validator over an untrusted `REPARSE_DATA_BUFFER` used for name grafting. It mirrors
//! what the IO manager's `IoIsValidNameGraftingBuffer` must do before
//! `IopDoNameTransmogrify`/`IopDoSymbolicLinkTransmogrify` perform any pointer
//! arithmetic on the substitute/print name ranges. The C correction (R1a, Stanley) is
//! the production path; this Rust validator is differentially tested against it.
//!
//! It removes the IO-3 defect class — offset/length arithmetic on attacker-influenced
//! bytes — *by construction*: every field read is bounds-checked (`rd_u16`/`rd_u32`
//! return `Option` and never index out of range) and every range test uses checked
//! subtraction, so no input can panic and the `panic=abort` bugcheck bridge is never
//! reached on data. One `unsafe` (`from_raw_parts`) with a documented SAFETY contract.
//!
//! # Buffer model (REPARSE_DATA_BUFFER, name-grafting variant)
//! ```text
//!   off  size  field
//!   0    4     ReparseTag            (u32 LE)
//!   4    2     ReparseDataLength     (u16 LE)  -- bytes AFTER this 8-byte fixed header
//!   6    2     Reserved              (ignored)
//!   8    .     per-tag field header + PathBuffer[]   (ReparseDataLength bytes)
//! ```
//! MOUNT_POINT (0xA0000003) field header = 8 bytes (4x u16); SYMLINK (0xA000000C)
//! field header = 12 bytes (4x u16 + u32 Flags). Name offsets/lengths are byte values
//! **relative to PathBuffer** (i.e. relative to the start of the path area that follows
//! the per-tag field header).
//!
//! # Invariants (STATUS_SUCCESS iff ALL hold) — authoritative; kept in sync with the C
//! # reference (`difftest/graft_ref_body.inc`) and the doc
//! # `tools/rust-poc/difftest/IO3-NAME-GRAFTING-INVARIANTS.md`:
//!   I1  `buf != NULL`                                           else INVALID_PARAMETER
//!   I2  `len >= 8` (fixed header fits) and `len <= 16384` (MAX) else IO_REPARSE_DATA_INVALID
//!   I3  embedded `ReparseTag == reparse_tag` argument          else IO_REPARSE_DATA_INVALID
//!   I4  `reparse_tag` is MOUNT_POINT or SYMLINK                else IO_REPARSE_TAG_NOT_HANDLED
//!   I5  `8 + ReparseDataLength == len` (buffer is exactly sized) else IO_REPARSE_DATA_INVALID
//!   I6  `ReparseDataLength >= field_header_size(tag)`           else IO_REPARSE_DATA_INVALID
//!   I7  all four name fields (SubstituteNameOffset/Length,
//!       PrintNameOffset/Length) are even (WCHAR-aligned)        else IO_REPARSE_DATA_INVALID
//!   I8  SubstituteName range within PathBuffer:
//!       `SubstituteNameOffset <= path_len` and
//!       `SubstituteNameLength <= path_len - SubstituteNameOffset` else IO_REPARSE_DATA_INVALID
//!   I9  PrintName range within PathBuffer (same shape)          else IO_REPARSE_DATA_INVALID
//!   I10 `SubstituteNameLength != 0` (a graft needs a target;
//!       PrintName may be empty)                                 else IO_REPARSE_DATA_INVALID
//! where `path_len = ReparseDataLength - field_header_size(tag)`.
//!
//! Content validation (embedded NUL, surrogate pairing) is deliberately **out of scope**
//! here: `IoIsValidNameGraftingBuffer` validates structure and ranges, not characters.
//! The NTFS on-disk validator (C1) does the surrogate/NUL checks on its own path.

use core::ffi::{c_ulong, c_void};
use core::slice;

/// NTSTATUS (LONG). Mirrors the C header's typedef.
pub type NtStatus = i32;

const STATUS_SUCCESS: NtStatus = 0x0000_0000u32 as i32;
const STATUS_INVALID_PARAMETER: NtStatus = 0xC000_000Du32 as i32;
const STATUS_IO_REPARSE_DATA_INVALID: NtStatus = 0xC000_0278u32 as i32;
const STATUS_IO_REPARSE_TAG_NOT_HANDLED: NtStatus = 0xC000_0279u32 as i32;

const IO_REPARSE_TAG_MOUNT_POINT: u32 = 0xA000_0003;
const IO_REPARSE_TAG_SYMLINK: u32 = 0xA000_000C;

const FIXED_HEADER_SIZE: u32 = 8; // ReparseTag(4) + ReparseDataLength(2) + Reserved(2)
const MOUNT_POINT_FIELD_HEADER: u32 = 8; // 4x USHORT
const SYMLINK_FIELD_HEADER: u32 = 12; // 4x USHORT + ULONG Flags
const MAX_REPARSE_DATA_BUFFER_SIZE: u32 = 16 * 1024; // IO_REPARSE_... maximum

// ---- bounded little-endian reads (never panic, never read out of bounds) ----

#[inline]
fn rd_u16(buf: &[u8], off: u32) -> Option<u16> {
    let o = off as usize;
    let b = buf.get(o..o.checked_add(2)?)?;
    Some(u16::from_le_bytes([b[0], b[1]]))
}

#[inline]
fn rd_u32(buf: &[u8], off: u32) -> Option<u32> {
    let o = off as usize;
    let b = buf.get(o..o.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// Core logic over a borrowed slice. Separated from the FFI shell so the differential
/// harness and the fuzz target exercise exactly the kernel logic with no `unsafe`.
pub fn validate_name_grafting(buf: &[u8], reparse_tag: u32) -> NtStatus {
    // I4: name grafting handles only MOUNT_POINT and SYMLINK.
    if reparse_tag != IO_REPARSE_TAG_MOUNT_POINT && reparse_tag != IO_REPARSE_TAG_SYMLINK {
        return STATUS_IO_REPARSE_TAG_NOT_HANDLED;
    }

    let len = buf.len() as u32;
    // I2: the 8-byte fixed header must fit and the buffer must not exceed the maximum.
    if len < FIXED_HEADER_SIZE || len > MAX_REPARSE_DATA_BUFFER_SIZE {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    // Fixed header. Reads are guaranteed in-range by the I2 length check, but we still
    // go through the checked readers so the function is panic-free by construction.
    let embedded_tag = match rd_u32(buf, 0) {
        Some(v) => v,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    let reparse_data_length = match rd_u16(buf, 4) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };

    // I3: the caller's expected tag must match the buffer's tag.
    if embedded_tag != reparse_tag {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    // I5: the declared length must account for exactly the rest of the buffer.
    // FIXED_HEADER_SIZE + reparse_data_length cannot overflow u32 (both small).
    if FIXED_HEADER_SIZE + reparse_data_length != len {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    let field_header_size = if reparse_tag == IO_REPARSE_TAG_MOUNT_POINT {
        MOUNT_POINT_FIELD_HEADER
    } else {
        SYMLINK_FIELD_HEADER
    };

    // I6: the declared data must at least cover the per-tag field header.
    if reparse_data_length < field_header_size {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let path_len = reparse_data_length - field_header_size;

    // The four USHORT name fields sit at the start of the field header (offset 8).
    let substitute_name_offset = match rd_u16(buf, FIXED_HEADER_SIZE) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    let substitute_name_length = match rd_u16(buf, FIXED_HEADER_SIZE + 2) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    let print_name_offset = match rd_u16(buf, FIXED_HEADER_SIZE + 4) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    let print_name_length = match rd_u16(buf, FIXED_HEADER_SIZE + 6) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };

    // I7: WCHAR alignment for every offset and length.
    if (substitute_name_offset | substitute_name_length | print_name_offset | print_name_length)
        & 1
        != 0
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    // I8/I9: each name range lies within PathBuffer, using checked subtraction so an
    // over-large offset can never make the length test wrap.
    if !range_within(substitute_name_offset, substitute_name_length, path_len) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if !range_within(print_name_offset, print_name_length, path_len) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    // I10: a graft must carry a non-empty substitute (target) name.
    if substitute_name_length == 0 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    STATUS_SUCCESS
}

/// `offset <= cap && length <= cap - offset`, computed without overflow/wraparound.
#[inline]
fn range_within(offset: u32, length: u32, cap: u32) -> bool {
    match cap.checked_sub(offset) {
        Some(remaining) => length <= remaining,
        None => false, // offset > cap
    }
}

/// C ABI: validate a name-grafting `REPARSE_DATA_BUFFER`.
///
/// Returns `STATUS_SUCCESS` iff every invariant above holds; otherwise one of
/// `STATUS_INVALID_PARAMETER`, `STATUS_IO_REPARSE_TAG_NOT_HANDLED` or
/// `STATUS_IO_REPARSE_DATA_INVALID`. Never allocates, never locks, never bugchecks on
/// any input; safe at IRQL <= DISPATCH_LEVEL (the create path calls at PASSIVE).
///
/// # Safety
/// `buf` must point to at least `len` readable bytes valid for the duration of the call,
/// or be NULL (which returns `STATUS_INVALID_PARAMETER`). The buffer is borrowed, never
/// retained, never freed: the C caller owns it and its pool lifetime, unchanged.
#[no_mangle]
pub unsafe extern "C" fn leeway_rs_poc_validate_reparse_graft(
    buf: *const c_void,
    len: c_ulong,
    reparse_tag: c_ulong,
) -> NtStatus {
    // I1: a NULL buffer is caller misuse, distinct from a malformed buffer.
    if buf.is_null() {
        return STATUS_INVALID_PARAMETER;
    }
    let total = len as usize;
    // SAFETY: caller guarantees `buf` is readable for `len` bytes (see header contract).
    let data = unsafe { slice::from_raw_parts(buf as *const u8, total) };
    validate_name_grafting(data, reparse_tag as u32)
}

// ===========================================================================
// R1a mirror: exact equivalent of Stanley's shipped C IopValidateMountPointBuffer
// (fix/io3-name-grafting ff2e252ff4d). THIS is the swap-equivalent used for the
// differential that gates a future C->Rust swap. It differs from
// validate_name_grafting above: MOUNT_POINT only, NULL -> DATA_INVALID (not
// INVALID_PARAMETER), len>=16, ReparseDataLength may be <= len-8 (trailing slack
// allowed, not exact), substitute/print ranges must not overlap when print!=0, and
// the first WCHAR of the substitute name must be L'\' (backslash). It returns only
// STATUS_SUCCESS or STATUS_IO_REPARSE_DATA_INVALID, matching his function exactly.
// Contract source: Stanley msg r1a-io3-meredith2 / god FWD 2026-11-13.
// ===========================================================================

const WCHAR_BACKSLASH: u16 = 0x005C; // L'\'

/// Core mount-point validation. Mirrors `IopValidateMountPointBuffer(buf, len)`:
/// SUCCESS iff every rule holds, else `STATUS_IO_REPARSE_DATA_INVALID`.
pub fn validate_mount_point(buf: &[u8]) -> NtStatus {
    let len = buf.len() as u32;
    if len < 16 {
        return STATUS_IO_REPARSE_DATA_INVALID; // fixed(8) + MP field header(8)
    }
    let tag = match rd_u32(buf, 0) {
        Some(v) => v,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    if tag != IO_REPARSE_TAG_MOUNT_POINT {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let rdl = match rd_u16(buf, 4) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    // 8 <= ReparseDataLength <= 16384-8  AND  ReparseDataLength <= len-8
    if rdl < MOUNT_POINT_FIELD_HEADER
        || rdl > MAX_REPARSE_DATA_BUFFER_SIZE - FIXED_HEADER_SIZE
        || rdl > len - FIXED_HEADER_SIZE
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let path_len = rdl - MOUNT_POINT_FIELD_HEADER;

    let so = match rd_u16(buf, 8) { Some(v) => v as u32, None => return STATUS_IO_REPARSE_DATA_INVALID };
    let sl = match rd_u16(buf, 10) { Some(v) => v as u32, None => return STATUS_IO_REPARSE_DATA_INVALID };
    let po = match rd_u16(buf, 12) { Some(v) => v as u32, None => return STATUS_IO_REPARSE_DATA_INVALID };
    let pl = match rd_u16(buf, 14) { Some(v) => v as u32, None => return STATUS_IO_REPARSE_DATA_INVALID };

    // all offsets and lengths even
    if ((so | sl | po | pl) & 1) != 0 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // off+len <= path, no wrap
    if !range_within(so, sl, path_len) || !range_within(po, pl, path_len) {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // substitute length >= one WCHAR
    if sl < 2 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // when a print name is present, the two ranges must not overlap
    if pl != 0 && so < po + pl && po < so + sl {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // first WCHAR of the substitute name must be a backslash. PathBuffer starts at
    // offset 16 (fixed 8 + MP field header 8); substitute name starts at +so.
    let first = match rd_u16(buf, FIXED_HEADER_SIZE + MOUNT_POINT_FIELD_HEADER + so) {
        Some(v) => v,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    if first != WCHAR_BACKSLASH {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    STATUS_SUCCESS
}

/// C ABI mirror of `IopValidateMountPointBuffer(PREPARSE_DATA_BUFFER buf, ULONG len)`.
/// Returns `STATUS_SUCCESS` or `STATUS_IO_REPARSE_DATA_INVALID` only. A NULL buffer is
/// `STATUS_IO_REPARSE_DATA_INVALID` (matching the C function, which treats NULL as a
/// malformed buffer, not caller misuse). Borrows the buffer; retains/allocates/frees
/// nothing. Safe at IRQL <= DISPATCH_LEVEL.
///
/// # Safety
/// `buf` must point to `len` readable bytes valid for the call, or be NULL.
#[no_mangle]
pub unsafe extern "C" fn leeway_rs_poc_validate_mount_point(
    buf: *const c_void,
    len: c_ulong,
) -> NtStatus {
    if buf.is_null() {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // SAFETY: caller guarantees `buf` is readable for `len` bytes (header contract).
    let data = unsafe { slice::from_raw_parts(buf as *const u8, len as usize) };
    validate_mount_point(data)
}
