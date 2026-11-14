//! LRM-RS — Driver Verifier contract stages (clean-room Rust).
//!
//! Clean-room Rust of the Driver-Verifier decision logic that the C enforces on a *verified*
//! driver (Dwight's `ntoskrnl/vf/vfirql.c`, `ntoskrnl/vf/vfpool.c` and `nvs/nt/ntdrvmgmt.c`,
//! observed via Ryan's `OBSERVED-CONTRACTS-SPECIAL-POOL-VERIFIER.md` and the C reference pinned at
//! `feat/special-pool 5f5f92ed47a`). OUR code — contracts observed, no source copied (owner rule).
//!
//! The pure decision cores return the exact `DRIVER_VERIFIER_DETECTED_VIOLATION` (0xC4) bug check
//! (sub-code in P1, current IRQL in P2) as a `BugCheck` value; the kernel shim raises
//! `KeBugCheckEx`, the host harness asserts it against the C stage code.
//!
//! Stage 2a: IRQL rules at the lock/wait calls. Stage 5: pool-check decisions, POOL_TYPE/FLAGS
//! decode, and pool tracking + the leak check at driver unload.

use crate::mm::BugCheck;

/// `DRIVER_VERIFIER_DETECTED_VIOLATION` (public bug-check code).
pub const BUGCHECK_DRIVER_VERIFIER: u32 = 0xC4;

// NT IRQLs (architecture constants).
const APC_LEVEL: u8 = 1;
const DISPATCH_LEVEL: u8 = 2;

// Stage 2a sub-codes (param1 of 0xC4), exactly as the C enforces them.
const VF_RELEASE_SPIN_LOCK_IRQL: u32 = 0x32;
const VF_ACQUIRE_FAST_MUTEX_IRQL: u32 = 0x33;
const VF_RELEASE_FAST_MUTEX_IRQL: u32 = 0x34;
const VF_WAIT_AT_DISPATCH: u32 = 0x3B;
const VF_ACQUIRE_AT_DPC_BELOW: u32 = 0x40;
const VF_RELEASE_FROM_DPC_BELOW: u32 = 0x41;
const VF_ACQUIRE_SPIN_LOCK_IRQL: u32 = 0x42;

/// `KeBugCheckEx(0xC4, SubCode, Irql, P3, P4)` — the verifier's parameter layout (sub-code in P1,
/// current IRQL in P2), distinct from special pool's (which carries the sub-code in P4).
#[inline]
fn viol(sub: u32, irql: u8, p3: usize, p4: usize) -> BugCheck {
    BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: sub as usize, p2: irql as usize, p3, p4 }
}

// ======================================================================================
// Stage 2a — IRQL rules (mirror of vfirql.c)
// ======================================================================================

/// `KeAcquireSpinLockAtDpcLevel`: must be AT or above DISPATCH. `Irql < DISPATCH` -> 0x40.
pub fn acquire_spinlock_at_dpc(irql: u8, lock: usize) -> Result<(), BugCheck> {
    if irql < DISPATCH_LEVEL {
        return Err(viol(VF_ACQUIRE_AT_DPC_BELOW, irql, lock, 0));
    }
    Ok(())
}

/// `KeReleaseSpinLockFromDpcLevel`: `Irql < DISPATCH` -> 0x41.
pub fn release_spinlock_from_dpc(irql: u8, lock: usize) -> Result<(), BugCheck> {
    if irql < DISPATCH_LEVEL {
        return Err(viol(VF_RELEASE_FROM_DPC_BELOW, irql, lock, 0));
    }
    Ok(())
}

/// `KeAcquireSpinLockRaiseToDpc` (x64): `Irql > DISPATCH` -> 0x42.
pub fn acquire_spinlock_raise_to_dpc(irql: u8, lock: usize) -> Result<(), BugCheck> {
    if irql > DISPATCH_LEVEL {
        return Err(viol(VF_ACQUIRE_SPIN_LOCK_IRQL, irql, lock, 0));
    }
    Ok(())
}

/// `KeReleaseSpinLock` (x64): must be exactly AT DISPATCH. `Irql != DISPATCH` -> 0x32.
pub fn release_spinlock(irql: u8, lock: usize) -> Result<(), BugCheck> {
    if irql != DISPATCH_LEVEL {
        return Err(viol(VF_RELEASE_SPIN_LOCK_IRQL, irql, lock, 0));
    }
    Ok(())
}

/// `ExAcquireFastMutex`: `Irql > APC` -> 0x33.
pub fn acquire_fast_mutex(irql: u8, mutex: usize) -> Result<(), BugCheck> {
    if irql > APC_LEVEL {
        return Err(viol(VF_ACQUIRE_FAST_MUTEX_IRQL, irql, mutex, 0));
    }
    Ok(())
}

/// `ExReleaseFastMutex`: must be exactly AT APC. `Irql != APC` -> 0x34; P3 = thread's
/// `CombinedApcDisable`, P4 = the mutex (the C's parameter order).
pub fn release_fast_mutex(irql: u8, mutex: usize, combined_apc_disable: usize) -> Result<(), BugCheck> {
    if irql != APC_LEVEL {
        return Err(viol(VF_RELEASE_FAST_MUTEX_IRQL, irql, combined_apc_disable, mutex));
    }
    Ok(())
}

/// `KeWaitFor{Single,Multiple}Objects`: a wait below DISPATCH is fine; a zero-timeout poll at
/// DISPATCH is fine; anything else at or above DISPATCH -> 0x3B. `timeout` is the caller's
/// `PLARGE_INTEGER` (0 = NULL); `timeout_zero` is `Timeout->QuadPart == 0`.
pub fn check_wait(irql: u8, object: usize, timeout: usize, timeout_zero: bool) -> Result<(), BugCheck> {
    if irql < DISPATCH_LEVEL {
        return Ok(());
    }
    if irql == DISPATCH_LEVEL && timeout != 0 && timeout_zero {
        return Ok(());
    }
    Err(viol(VF_WAIT_AT_DISPATCH, irql, object, timeout))
}

// ======================================================================================
// Stage 5 — pool-check decisions, POOL_TYPE/FLAGS decode, tracking + leak (mirror of vfpool.c)
// ======================================================================================

// Stage 5 sub-codes (param1 of 0xC4).
const VF_UNLOAD_WITH_POOL: u32 = 0x62;
const VF_ALLOCATE_ZERO_BYTES: u32 = 0x00;
const VF_ALLOCATE_PAGED_IRQL: u32 = 0x01;
const VF_ALLOCATE_IRQL: u32 = 0x02;
const VF_FREE_PAGED_IRQL: u32 = 0x11;
const VF_FREE_IRQL: u32 = 0x12;

// POOL_TYPE values (public NT): NonPagedPool/NonPagedPoolExecute = 0, PagedPool = 1,
// NonPagedPoolNx = 0x200. Cache alignment OR'd via CACHE_ALIGNED_POOL_MASK (the full-stack feeds
// both sides the identical value, so the equivalence is value-independent of it).
const POOL_PAGED: u32 = 1;
const POOL_NONPAGED_EXECUTE: u32 = 0;
const POOL_NONPAGED_NX: u32 = 0x200;
const CACHE_ALIGNED_POOL_MASK: u32 = 4;
const NONPAGED_POOL_ID: usize = 0; // bug-check param: NonPagedPool
const PAGED_POOL_ID: usize = 1; //    bug-check param: PagedPool

// POOL_FLAGS bits (public WDM); only the type/alignment/zero bits the decode consults.
const POOL_FLAG_UNINITIALIZED: u32 = 0x02;
const POOL_FLAG_CACHE_ALIGNED: u32 = 0x08;
const POOL_FLAG_NON_PAGED: u32 = 0x40;
const POOL_FLAG_NON_PAGED_EXECUTE: u32 = 0x80;
const POOL_FLAG_PAGED: u32 = 0x100;

// EX_POOL_PRIORITY special-pool low nibble (public): overrun = 8, underrun = 9.
const PRIORITY_SPECIAL_OVERRUN: u32 = 8;
const PRIORITY_SPECIAL_UNDERRUN: u32 = 9;

/// `VfpCheckAllocate`: zero bytes -> 0x00; paged at IRQL > APC -> 0x01; any at IRQL > DISPATCH ->
/// 0x02. `KeBugCheckEx(0xC4, Code, Irql, PoolType, Bytes)`.
pub fn check_allocate(pool_type: u32, bytes: usize, irql: u8) -> Result<(), BugCheck> {
    let code = if bytes == 0 {
        VF_ALLOCATE_ZERO_BYTES
    } else if (pool_type & 1) != 0 && irql > APC_LEVEL {
        VF_ALLOCATE_PAGED_IRQL
    } else if irql > DISPATCH_LEVEL {
        VF_ALLOCATE_IRQL
    } else {
        return Ok(());
    };
    Err(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: code as usize, p2: irql as usize, p3: pool_type as usize, p4: bytes })
}

/// `VfpCheckFree`: IRQL > DISPATCH -> 0x12 (P3 = NonPagedPool); else IRQL > APC on a tracked paged
/// block -> 0x11 (P3 = PagedPool). P4 = block.
pub fn check_free(irql: u8, tracked_paged: bool, block: usize) -> Result<(), BugCheck> {
    if irql > DISPATCH_LEVEL {
        return Err(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: VF_FREE_IRQL as usize, p2: irql as usize, p3: NONPAGED_POOL_ID, p4: block });
    }
    if irql > APC_LEVEL && tracked_paged {
        return Err(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: VF_FREE_PAGED_IRQL as usize, p2: irql as usize, p3: PAGED_POOL_ID, p4: block });
    }
    Ok(())
}

/// `VfpSpecialType`: the special-pool placement from an `EX_POOL_PRIORITY` (low nibble): overrun
/// (8) -> 0 (page end), underrun (9) -> 1 (page start), else -> 2 (global default).
pub fn special_type(priority: u32) -> u32 {
    match priority & 0xF {
        PRIORITY_SPECIAL_OVERRUN => 0,
        PRIORITY_SPECIAL_UNDERRUN => 1,
        _ => 2,
    }
}

/// `VfpAllocatePool2` POOL_FLAGS decode: `None` (route to the normal allocator) if `tag == 0`, no
/// type bit, or more than one type bit; else the decoded `POOL_TYPE` (paged / non-paged-execute /
/// non-paged-nx), OR'd with the cache-align mask.
pub fn pool2_decode(flags: u32, tag: u32) -> Option<u32> {
    let type_flags = flags & (POOL_FLAG_NON_PAGED | POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_PAGED);
    if tag == 0 || type_flags == 0 || (type_flags & (type_flags - 1)) != 0 {
        return None;
    }
    let mut pool_type = if (type_flags & POOL_FLAG_PAGED) != 0 {
        POOL_PAGED
    } else if (type_flags & POOL_FLAG_NON_PAGED_EXECUTE) != 0 {
        POOL_NONPAGED_EXECUTE
    } else {
        POOL_NONPAGED_NX
    };
    if (flags & POOL_FLAG_CACHE_ALIGNED) != 0 {
        pool_type |= CACHE_ALIGNED_POOL_MASK;
    }
    Some(pool_type)
}

/// `VfpAllocatePool2` zero-on-alloc: zero unless `POOL_FLAG_UNINITIALIZED` is set.
pub fn pool2_zero(flags: u32) -> bool {
    (flags & POOL_FLAG_UNINITIALIZED) == 0
}

/// One tracked allocation (mirror of the kernel's per-block verifier record).
#[derive(Clone, Copy, Default)]
pub struct TrackEntry {
    pub block: usize,
    pub caller: usize,
    pub size: u32,
    pub paged: bool,
    pub used: bool,
}

/// A fixed-capacity, no-heap mirror of the verifier pool-tracking table (`VfpTrack` /
/// `VfUntrackPool` / `VfCheckPoolAtUnload`). The kernel uses a hashed list of heap entries; a
/// clean-room Rust chooses its own storage, so we use a caller-provided slice. The counters mirror
/// the per-paged accounting in `MmVerifierData`.
pub struct PoolTracker<'a> {
    entries: &'a mut [TrackEntry],
    pub paged_bytes: u32,
    pub nonpaged_bytes: u32,
    pub cur_paged: u32,
    pub cur_nonpaged: u32,
    pub peak_paged: u32,
    pub peak_nonpaged: u32,
    pub peak_paged_bytes: u32,
    pub peak_nonpaged_bytes: u32,
    pub untracked: u32,
}

impl<'a> PoolTracker<'a> {
    pub fn new(storage: &'a mut [TrackEntry]) -> Self {
        for e in storage.iter_mut() {
            *e = TrackEntry::default();
        }
        PoolTracker {
            entries: storage,
            paged_bytes: 0,
            nonpaged_bytes: 0,
            cur_paged: 0,
            cur_nonpaged: 0,
            peak_paged: 0,
            peak_nonpaged: 0,
            peak_paged_bytes: 0,
            peak_nonpaged_bytes: 0,
            untracked: 0,
        }
    }

    /// Insert a tracked allocation; bumps per-paged counters and peaks. A full table counts as
    /// untracked (mirrors the C's track-allocation-failure path).
    pub fn track(&mut self, block: usize, size: u32, paged: bool, caller: usize) {
        let free = self.entries.iter_mut().find(|e| !e.used);
        let e = match free {
            Some(e) => e,
            None => {
                self.untracked = self.untracked.wrapping_add(1);
                return;
            }
        };
        *e = TrackEntry { block, caller, size, paged, used: true };
        if paged {
            self.paged_bytes = self.paged_bytes.wrapping_add(size);
            self.cur_paged += 1;
            if self.cur_paged > self.peak_paged {
                self.peak_paged = self.cur_paged;
            }
            if self.paged_bytes > self.peak_paged_bytes {
                self.peak_paged_bytes = self.paged_bytes;
            }
        } else {
            self.nonpaged_bytes = self.nonpaged_bytes.wrapping_add(size);
            self.cur_nonpaged += 1;
            if self.cur_nonpaged > self.peak_nonpaged {
                self.peak_nonpaged = self.cur_nonpaged;
            }
            if self.nonpaged_bytes > self.peak_nonpaged_bytes {
                self.peak_nonpaged_bytes = self.nonpaged_bytes;
            }
        }
    }

    /// Remove a tracked block (no-op if absent); reverses the current counters (not the peaks).
    pub fn untrack(&mut self, block: usize) {
        if let Some(e) = self.entries.iter_mut().find(|e| e.used && e.block == block) {
            if e.paged {
                self.cur_paged -= 1;
                self.paged_bytes = self.paged_bytes.wrapping_sub(e.size);
            } else {
                self.cur_nonpaged -= 1;
                self.nonpaged_bytes = self.nonpaged_bytes.wrapping_sub(e.size);
            }
            *e = TrackEntry::default();
        }
    }

    /// Is `block` a tracked paged allocation?
    pub fn is_tracked_paged(&self, block: usize) -> bool {
        self.entries
            .iter()
            .find(|e| e.used && e.block == block)
            .map(|e| e.paged)
            .unwrap_or(false)
    }

    /// `VfCheckPoolAtUnload`: count entries whose caller lies in `[start, end)`.
    pub fn leak_count(&self, start: usize, end: usize) -> u32 {
        let mut n = 0u32;
        for e in self.entries.iter() {
            if e.used && e.caller >= start && e.caller < end {
                n += 1;
            }
        }
        n
    }

    /// The unload leak check as the C raises it: non-zero leaks -> `0xC4 (0x62, name, 0, count)`.
    pub fn check_at_unload(&self, name: usize, start: usize, end: usize) -> Result<(), BugCheck> {
        let leaked = self.leak_count(start, end);
        if leaked != 0 {
            return Err(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: VF_UNLOAD_WITH_POOL as usize, p2: name, p3: 0, p4: leaked as usize });
        }
        Ok(())
    }
}

// ======================================================================================
// C ABI for the host differentials
// ======================================================================================

macro_rules! abi_irql1 {
    ($name:ident, $rule:ident) => {
        /// # Safety
        /// `out` must be a writable `BugCheck`.
        #[no_mangle]
        pub unsafe extern "C" fn $name(irql: u8, p: usize, out: *mut BugCheck) -> u8 {
            match $rule(irql, p) {
                Ok(()) => 0,
                Err(bc) => {
                    if !out.is_null() {
                        unsafe { *out = bc };
                    }
                    1
                }
            }
        }
    };
}

abi_irql1!(lrm_vf_acquire_spinlock_at_dpc, acquire_spinlock_at_dpc);
abi_irql1!(lrm_vf_release_spinlock_from_dpc, release_spinlock_from_dpc);
abi_irql1!(lrm_vf_acquire_spinlock_raise_to_dpc, acquire_spinlock_raise_to_dpc);
abi_irql1!(lrm_vf_release_spinlock, release_spinlock);
abi_irql1!(lrm_vf_acquire_fast_mutex, acquire_fast_mutex);

#[inline]
fn write_bc(r: Result<(), BugCheck>, out: *mut BugCheck) -> u8 {
    match r {
        Ok(()) => 0,
        Err(bc) => {
            if !out.is_null() {
                // SAFETY: caller guarantees `out` writable.
                unsafe { *out = bc };
            }
            1
        }
    }
}

/// `ExReleaseFastMutex` classifier.
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_release_fast_mutex(irql: u8, mutex: usize, combined_apc_disable: usize, out: *mut BugCheck) -> u8 {
    write_bc(release_fast_mutex(irql, mutex, combined_apc_disable), out)
}

/// Wait classifier.
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_wait(irql: u8, object: usize, timeout: usize, timeout_zero: u8, out: *mut BugCheck) -> u8 {
    write_bc(check_wait(irql, object, timeout, timeout_zero != 0), out)
}

/// `VfpCheckAllocate` classifier.
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_allocate(pool_type: u32, bytes: usize, irql: u8, out: *mut BugCheck) -> u8 {
    write_bc(check_allocate(pool_type, bytes, irql), out)
}

/// `VfpCheckFree` classifier.
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_free(irql: u8, tracked_paged: u8, block: usize, out: *mut BugCheck) -> u8 {
    write_bc(check_free(irql, tracked_paged != 0, block), out)
}

/// `VfpSpecialType(Priority)`.
#[no_mangle]
pub extern "C" fn lrm_vf_special_type(priority: u32) -> u32 {
    special_type(priority)
}

/// `VfpAllocatePool2` decode: returns 1 and writes the decoded pool type to `*out_type` if the
/// flags select special pool, else 0 (route to the normal allocator). `*out_zero` gets the zero bit.
///
/// # Safety
/// `out_type` and `out_zero` must be writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_pool2_decode(flags: u32, tag: u32, out_type: *mut u32, out_zero: *mut u8) -> u8 {
    if !out_zero.is_null() {
        unsafe { *out_zero = pool2_zero(flags) as u8 };
    }
    match pool2_decode(flags, tag) {
        Some(pt) => {
            if !out_type.is_null() {
                unsafe { *out_type = pt };
            }
            1
        }
        None => 0,
    }
}

/// Externally-owned counter state for the pool tracker over the C ABI, mirroring the per-paged
/// accounting in `MmVerifierData`. The caller also owns the `TrackEntry` buffer; both persist
/// across calls (zero-initialise once).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct VfTrackState {
    pub paged_bytes: u32,
    pub nonpaged_bytes: u32,
    pub cur_paged: u32,
    pub cur_nonpaged: u32,
    pub peak_paged: u32,
    pub peak_nonpaged: u32,
    pub peak_paged_bytes: u32,
    pub peak_nonpaged_bytes: u32,
    pub untracked: u32,
}

#[inline]
unsafe fn with_tracker<R>(
    st: *mut VfTrackState,
    entries: *mut TrackEntry,
    cap: usize,
    f: impl FnOnce(&mut PoolTracker) -> R,
) -> R {
    // SAFETY: caller guarantees `entries`/`st` valid for `cap` entries and the lifetime of the call.
    let slice = unsafe { core::slice::from_raw_parts_mut(entries, cap) };
    let s = unsafe { *st };
    let mut t = PoolTracker {
        entries: slice,
        paged_bytes: s.paged_bytes,
        nonpaged_bytes: s.nonpaged_bytes,
        cur_paged: s.cur_paged,
        cur_nonpaged: s.cur_nonpaged,
        peak_paged: s.peak_paged,
        peak_nonpaged: s.peak_nonpaged,
        peak_paged_bytes: s.peak_paged_bytes,
        peak_nonpaged_bytes: s.peak_nonpaged_bytes,
        untracked: s.untracked,
    };
    let r = f(&mut t);
    unsafe {
        *st = VfTrackState {
            paged_bytes: t.paged_bytes,
            nonpaged_bytes: t.nonpaged_bytes,
            cur_paged: t.cur_paged,
            cur_nonpaged: t.cur_nonpaged,
            peak_paged: t.peak_paged,
            peak_nonpaged: t.peak_nonpaged,
            peak_paged_bytes: t.peak_paged_bytes,
            peak_nonpaged_bytes: t.peak_nonpaged_bytes,
            untracked: t.untracked,
        };
    }
    r
}

/// `VfpTrack` over the C ABI.
/// # Safety
/// `st` and `entries` (for `cap` entries) must be valid and writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_track(st: *mut VfTrackState, entries: *mut TrackEntry, cap: usize, block: usize, size: u32, paged: u8, caller: usize) {
    unsafe { with_tracker(st, entries, cap, |t| t.track(block, size, paged != 0, caller)) }
}

/// `VfUntrackPool` over the C ABI.
/// # Safety
/// `st` and `entries` (for `cap` entries) must be valid and writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_untrack(st: *mut VfTrackState, entries: *mut TrackEntry, cap: usize, block: usize) {
    unsafe { with_tracker(st, entries, cap, |t| t.untrack(block)) }
}

/// `VfpIsTrackedPaged` over the C ABI.
/// # Safety
/// `entries` must be valid for `cap` entries.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_is_tracked_paged(entries: *const TrackEntry, cap: usize, block: usize) -> u8 {
    let slice = unsafe { core::slice::from_raw_parts(entries, cap) };
    slice.iter().find(|e| e.used && e.block == block).map(|e| e.paged).unwrap_or(false) as u8
}

/// `VfCheckPoolAtUnload` over the C ABI: 1 and `*out` set if a leak bug check is due, else 0.
/// # Safety
/// `entries` (for `cap`) and `out` must be valid.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_at_unload(entries: *const TrackEntry, cap: usize, name: usize, start: usize, end: usize, out: *mut BugCheck) -> u8 {
    let slice = unsafe { core::slice::from_raw_parts(entries, cap) };
    let mut leaked = 0u32;
    for e in slice.iter() {
        if e.used && e.caller >= start && e.caller < end {
            leaked += 1;
        }
    }
    if leaked != 0 {
        if !out.is_null() {
            unsafe { *out = BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: VF_UNLOAD_WITH_POOL as usize, p2: name, p3: 0, p4: leaked as usize } };
        }
        return 1;
    }
    0
}
