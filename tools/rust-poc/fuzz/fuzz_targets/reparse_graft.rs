#![no_main]
//! Differential libFuzzer target for the IO-3 mount-point validator.
//!
//! Exercises the EXACT kernel validator source (`src/reparse_graft.rs`, via `#[path]`) and
//! compares `validate_mount_point` to an independent in-file oracle (`oracle`, different
//! style) for the same input. Any divergence or panic fails the run. Mirrors Stanley's
//! shipped contract at `1d45eab8da4` (overlap rule dropped).
//!
//! Run on the Linux private CI:
//!   cargo +nightly-2026-05-24 fuzz run reparse_graft -- -max_total_time=300
//! Seed corpus: fuzz/corpus/reparse_graft/.

use libfuzzer_sys::fuzz_target;

#[path = "../../src/reparse_graft.rs"]
mod reparse_graft;

const MOUNT_POINT: u32 = 0xA000_0003;
const STATUS_SUCCESS: i32 = 0x0000_0000u32 as i32;
const STATUS_IO_REPARSE_DATA_INVALID: i32 = 0xC000_0278u32 as i32;

/// Independent oracle of the mount-point contract (V1-V7, overlap dropped). `buf` is a
/// non-null borrowed slice, so the NULL case is covered by the C++ differential, not here.
fn oracle(buf: &[u8]) -> i32 {
    let len = buf.len();
    if len < 16 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let rd16 = |o: usize| u16::from_le_bytes([buf[o], buf[o + 1]]) as usize;
    let rd32 = |o: usize| u32::from_le_bytes([buf[o], buf[o + 1], buf[o + 2], buf[o + 3]]);
    if rd32(0) != MOUNT_POINT {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let rdl = rd16(4);
    if rdl < 8 || rdl > 16 * 1024 - 8 || rdl > len - 8 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    let path = rdl - 8;
    let so = rd16(8);
    let sl = rd16(10);
    let po = rd16(12);
    let pl = rd16(14);
    if ((so | sl | po | pl) & 1) != 0 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if so > path || sl > path - so || po > path || pl > path - po {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if sl < 2 {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    if rd16(16 + so) != 0x005C {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }
    STATUS_SUCCESS
}

fuzz_target!(|data: &[u8]| {
    let got = reparse_graft::validate_mount_point(data);
    let want = oracle(data);
    assert_eq!(
        got, want,
        "divergence: len={} got=0x{:08x} want=0x{:08x}",
        data.len(),
        got,
        want
    );
    // The validator returns only these two statuses.
    assert!(got == STATUS_SUCCESS || got == STATUS_IO_REPARSE_DATA_INVALID);
});
