#![no_main]
//! Differential libFuzzer target for the IO-3 name-grafting validator.
//!
//! Exercises the EXACT kernel validator source (`src/reparse_graft.rs`, via `#[path]`)
//! and compares it to an independent in-file oracle (`oracle_validate`, written in a
//! different style) for the same input under three tags: the tag embedded in the first
//! four bytes, MOUNT_POINT, and SYMLINK. Any divergence or panic fails the run.
//!
//! Run on the Linux private CI:
//!   cargo +nightly-2026-05-24 fuzz run reparse_graft -- -max_total_time=300
//! Seed corpus: fuzz/corpus/reparse_graft/.

use libfuzzer_sys::fuzz_target;

#[path = "../../src/reparse_graft.rs"]
mod reparse_graft;

const MOUNT_POINT: u32 = 0xA000_0003;
const SYMLINK: u32 = 0xA000_000C;
const STATUS_SUCCESS: i32 = 0x0000_0000u32 as i32;
const STATUS_INVALID_PARAMETER: i32 = 0xC000_000Du32 as i32;
const STATUS_IO_REPARSE_DATA_INVALID: i32 = 0xC000_0278u32 as i32;
const STATUS_IO_REPARSE_TAG_NOT_HANDLED: i32 = 0xC000_0279u32 as i32;

/// Independent oracle, deliberately not sharing code with the validator under test.
/// `buf` is always non-null here (a borrowed slice), so the NULL/INVALID_PARAMETER case
/// is covered separately by the C++ differential harness, not here.
fn oracle_validate(buf: &[u8], tag: u32) -> i32 {
    if tag != MOUNT_POINT && tag != SYMLINK {
        return STATUS_IO_REPARSE_TAG_NOT_HANDLED;
    }
    let len = buf.len();
    if len < 8 || len > 16 * 1024 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let rd16 = |o: usize| u16::from_le_bytes([buf[o], buf[o + 1]]) as usize;
    let rd32 = |o: usize| u32::from_le_bytes([buf[o], buf[o + 1], buf[o + 2], buf[o + 3]]);
    let etag = rd32(0);
    let rdl = rd16(4);
    if etag != tag {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if 8 + rdl != len {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let fh = if tag == MOUNT_POINT { 8 } else { 12 };
    if rdl < fh {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let path_len = rdl - fh;
    let so = rd16(8);
    let sl = rd16(10);
    let po = rd16(12);
    let pl = rd16(14);
    if ((so | sl | po | pl) & 1) != 0 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if so > path_len || sl > path_len - so {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if po > path_len || pl > path_len - po {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if sl == 0 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    STATUS_SUCCESS
}

fn one(buf: &[u8], tag: u32) {
    let got = reparse_graft::validate_name_grafting(buf, tag);
    let want = oracle_validate(buf, tag);
    assert_eq!(
        got, want,
        "divergence: len={} tag=0x{:x} got=0x{:08x} want=0x{:08x}",
        buf.len(),
        tag,
        got,
        want
    );
    // A successful validation must never be INVALID_PARAMETER here (buf is non-null).
    debug_assert_ne!(got, STATUS_INVALID_PARAMETER);
}

fuzz_target!(|data: &[u8]| {
    let embedded = if data.len() >= 4 {
        u32::from_le_bytes([data[0], data[1], data[2], data[3]])
    } else {
        0
    };
    one(data, embedded);
    one(data, MOUNT_POINT);
    one(data, SYMLINK);
});
