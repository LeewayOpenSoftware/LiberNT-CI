//! R1b — IO-3 mount-point reparse validator (experimental Rust, behind a C ABI).
//!
//! The first real lnrspoc payload: an alloc-free, lock-free, panic-free validator over an
//! untrusted `REPARSE_DATA_BUFFER`, mirroring the **shipped C** correction
//! `IopValidateMountPointBuffer` (Stanley, `fix/io3-name-grafting` `1d45eab8da4`). The C
//! fix is the production path and THE contract; this Rust is differentially tested against
//! it. It removes the IO-3 defect class — offset/length arithmetic on attacker-influenced
//! bytes — by construction (every read is bounds-checked via `rd_u16`/`rd_u32`, every range
//! test uses checked subtraction), so no input can panic and the `panic=abort` bugcheck
//! bridge is never reached on data. One `unsafe` (`from_raw_parts`) with the SAFETY contract
//! below.
//!
//! # Buffer model (REPARSE_DATA_BUFFER, mount-point variant)
//! ```text
//!   off  size  field
//!   0    4     ReparseTag            (u32 LE)  -- must be IO_REPARSE_TAG_MOUNT_POINT
//!   4    2     ReparseDataLength     (u16 LE)  -- bytes AFTER this 8-byte fixed header
//!   6    2     Reserved              (ignored)
//!   8    8     SubstituteNameOffset, SubstituteNameLength, PrintNameOffset,
//!              PrintNameLength (4x u16 LE) -- the MOUNT_POINT field header
//!   16   .     PathBuffer[]          (ReparseDataLength - 8 bytes of path area)
//! ```
//! Name offsets/lengths are byte values **relative to PathBuffer** (byte 16).
//!
//! # Invariants — authoritative contract, mirroring `IopValidateMountPointBuffer`
//! (`1d45eab8da4`, after the overlap rule was dropped). Returns ONLY `STATUS_SUCCESS` or
//! `STATUS_IO_REPARSE_DATA_INVALID`. SUCCESS iff ALL hold:
//!   V1  buf != NULL (a NULL buffer is a malformed buffer here, not INVALID_PARAMETER);
//!   V2  len >= 16;
//!   V3  ReparseTag == IO_REPARSE_TAG_MOUNT_POINT (MOUNT_POINT only — symlinks are
//!       validated separately; any other tag is INVALID, not TAG_NOT_HANDLED);
//!   V4  8 <= ReparseDataLength <= 16384 - 8  and  ReparseDataLength <= len - 8
//!       (len is an UPPER BOUND — trailing slack is allowed, not an exact size);
//!   V5  with path = ReparseDataLength - 8: SubstituteName and PrintName offsets and
//!       lengths are all even and offset + length <= path (checked, no wrap);
//!   V6  SubstituteNameLength >= 2 (a mount point needs a non-empty target);
//!   V7  the first WCHAR of the substitute name is a backslash (0x005C) — an absolute path.
//! There is intentionally **no** substitute/print overlap rule (dropped at `1d45eab8da4`:
//! aliased names are legal) and **no** character/surrogate validation (out of scope for
//! this entry point). A SYMLINK validator, if ever wanted, is a separate function.

use core::ffi::c_void;
use core::slice;

/// NTSTATUS (LONG). The kernel ULONG/LONG are 32-bit (LLP64); this ABI uses fixed 32-bit
/// widths so the host differential/fuzz exercise the exact signature the driver links.
pub type NtStatus = i32;

const STATUS_SUCCESS: NtStatus = 0x0000_0000u32 as i32;
const STATUS_IO_REPARSE_DATA_INVALID: NtStatus = 0xC000_0278u32 as i32;

const IO_REPARSE_TAG_MOUNT_POINT: u32 = 0xA000_0003;
const FIXED_HEADER_SIZE: u32 = 8; // ReparseTag(4) + ReparseDataLength(2) + Reserved(2)
const MOUNT_POINT_FIELD_HEADER: u32 = 8; // 4x USHORT
const MAX_REPARSE_DATA_BUFFER_SIZE: u32 = 16 * 1024;
const WCHAR_BACKSLASH: u16 = 0x005C; // L backslash

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

/// `offset <= cap && length <= cap - offset`, computed without overflow/wraparound.
#[inline]
fn range_within(offset: u32, length: u32, cap: u32) -> bool {
    match cap.checked_sub(offset) {
        Some(remaining) => length <= remaining,
        None => false,
    }
}

/// Core logic over a borrowed slice. Separated from the FFI shell so the differential
/// harness and the fuzz target exercise exactly the kernel logic with no `unsafe`.
/// Returns `STATUS_SUCCESS` or `STATUS_IO_REPARSE_DATA_INVALID` only.
pub fn validate_mount_point(buf: &[u8]) -> NtStatus {
    // A kernel buffer never exceeds u32, but fail cleanly rather than truncate (Dwight NIT).
    let len = match u32::try_from(buf.len()) {
        Ok(v) => v,
        Err(_) => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    if len < FIXED_HEADER_SIZE + MOUNT_POINT_FIELD_HEADER {
        return STATUS_IO_REPARSE_DATA_INVALID; // V2: len >= 16
    }
    let tag = match rd_u32(buf, 0) {
        Some(v) => v,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    if tag != IO_REPARSE_TAG_MOUNT_POINT {
        return STATUS_IO_REPARSE_DATA_INVALID; // V3
    }
    let rdl = match rd_u16(buf, 4) {
        Some(v) => v as u32,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    // V4: 8 <= rdl <= 16384-8  AND  rdl <= len-8 (len is an upper bound, not exact).
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

    if ((so | sl | po | pl) & 1) != 0 {
        return STATUS_IO_REPARSE_DATA_INVALID; // V5: even
    }
    if !range_within(so, sl, path_len) || !range_within(po, pl, path_len) {
        return STATUS_IO_REPARSE_DATA_INVALID; // V5: within path
    }
    if sl < 2 {
        return STATUS_IO_REPARSE_DATA_INVALID; // V6: non-empty substitute
    }
    // V7: first WCHAR of the substitute name is a backslash. PathBuffer is at byte 16.
    let first = match rd_u16(buf, FIXED_HEADER_SIZE + MOUNT_POINT_FIELD_HEADER + so) {
        Some(v) => v,
        None => return STATUS_IO_REPARSE_DATA_INVALID,
    };
    if first != WCHAR_BACKSLASH {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    STATUS_SUCCESS
}

/// C ABI mirror of `NTSTATUS IopValidateMountPointBuffer(PREPARSE_DATA_BUFFER buf, ULONG len)`.
/// Returns `STATUS_SUCCESS` or `STATUS_IO_REPARSE_DATA_INVALID` only; a NULL buffer is
/// `STATUS_IO_REPARSE_DATA_INVALID`. `len` is `u32` to match the kernel `ULONG` (32-bit)
/// exactly on both the kernel target and the LP64 host where the differential runs.
///
/// # Safety
/// `buf` must either be NULL, or point to at least `len` bytes that for the whole call are:
/// (1) **kernel** memory, never a user-mode pointer; (2) a captured/private copy that no
/// other thread writes during the call (the lnrspoc IOCTL is METHOD_BUFFERED, so the driver
/// passes the kernel SystemBuffer copy); (3) mapped and readable — **non-paged** if the
/// caller is above APC_LEVEL. Under those conditions the function reads the buffer, retains
/// nothing, allocates/frees nothing, takes no lock, and is safe at IRQL <= DISPATCH_LEVEL.
/// It never raises; callers must still never let an exception cross this frame (the function
/// touches only the given bytes and calls nothing that raises).
#[no_mangle]
pub unsafe extern "C" fn leeway_rs_poc_validate_mount_point(
    buf: *const c_void,
    len: u32,
) -> NtStatus {
    if buf.is_null() {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    // SAFETY: per the contract above, `buf` is readable for `len` kernel bytes for the call.
    let data = unsafe { slice::from_raw_parts(buf as *const u8, len as usize) };
    validate_mount_point(data)
}
