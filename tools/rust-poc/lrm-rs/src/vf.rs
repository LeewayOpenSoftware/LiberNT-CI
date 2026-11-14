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
    // p3 = PoolType: C casts a signed POOL_TYPE enum into the 0xC4 parameter, so a value with bit 31
    // set sign-extends; mirror that for exact parity (not `pool_type as usize`, which zero-extends).
    Err(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: code as usize, p2: irql as usize, p3: pool_type as i32 as isize as usize, p4: bytes })
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
/// One tracked pool allocation. `#[repr(C)]` because the caller (host differential / in-kernel
/// shim) owns and lays out the buffer; `paged`/`used` are `u8` (0/1), not Rust `bool`, so no Rust
/// bool crosses the ABI. `tag` is the pool tag, kept so the leak report ("did not free") lines can
/// name the tag; those lines are produced by the caller OUTSIDE the tracking lock (this module only
/// returns the bug-check value).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TrackEntry {
    pub block: usize,
    pub caller: usize,
    pub size: u32,
    pub tag: u32,
    pub paged: u8,
    pub used: u8,
}

/// A no-heap mirror of the verifier pool-tracking table (`VfpTrack` / `VfUntrackPool` /
/// `VfCheckPoolAtUnload`). The kernel uses a hashed list of heap entries with no fixed limit; a
/// clean-room Rust uses a caller-provided slice. The counters mirror the per-paged accounting in
/// `MmVerifierData`.
///
/// Contract (the caller — host harness or in-kernel shim — must honour it):
/// - LOCKING: this table is NOT internally synchronised. The caller holds the verifier pool spin
///   lock across every call, exactly as the C does; there is no interior mutability or atomics.
/// - CAPACITY / NEVER SILENTLY DROP: the C table is unbounded, so the caller MUST size `entries` to
///   at least the maximum number of concurrently-tracked allocations. If the slice ever fills,
///   `track` cannot record the block and bumps `untracked` instead of dropping it silently;
///   `untracked != 0` is a fail-closed signal that the leak (`0x62`) and tracked-paged-free (`0x11`)
///   results are no longer complete, and the caller must treat it as a tracking failure (size the
///   buffer up) rather than trust an under-count. It is never silent: the count is observable.
/// - LOOKUP: `track`/`untrack`/`is_tracked_paged` are hash-indexed (open addressing: start at the
///   block's hash slot, then a bounded ring fallback), so the per-free path at DISPATCH_LEVEL does
///   not linear-scan the whole table on a hit. The unload sweep (`leak_count`) is O(n) by nature,
///   as it is in the C.
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

    // Home slot of a block (open addressing). 12 = PAGE_SHIFT; mixes page and sub-page bits like the
    // C VfpTrack hash. 0 for an empty table (callers guard cap == 0).
    #[inline]
    fn home(&self, block: usize) -> usize {
        let cap = self.entries.len();
        if cap == 0 { 0 } else { ((block >> 12) ^ (block >> 4)) % cap }
    }

    // First free slot at/after the home slot (full ring); None if the table is full.
    fn find_free(&self, block: usize) -> Option<usize> {
        let cap = self.entries.len();
        if cap == 0 {
            return None;
        }
        let h = self.home(block);
        (0..cap).map(|k| (h + k) % cap).find(|&i| self.entries[i].used == 0)
    }

    // The used slot holding `block`, searched from its home over the FULL ring (so a prior cleared
    // slot cannot hide a later colliding entry); None if absent.
    fn find_used(&self, block: usize) -> Option<usize> {
        let cap = self.entries.len();
        if cap == 0 {
            return None;
        }
        let h = self.home(block);
        (0..cap)
            .map(|k| (h + k) % cap)
            .find(|&i| self.entries[i].used != 0 && self.entries[i].block == block)
    }

    /// Insert a tracked allocation; bumps per-paged counters and peaks. A full table does NOT drop
    /// silently: it bumps `untracked` (the fail-closed signal; see the struct contract).
    pub fn track(&mut self, block: usize, size: u32, paged: bool, caller: usize, tag: u32) {
        let i = match self.find_free(block) {
            Some(i) => i,
            None => {
                self.untracked = self.untracked.saturating_add(1);
                return;
            }
        };
        self.entries[i] = TrackEntry { block, caller, size, tag, paged: paged as u8, used: 1 };
        if paged {
            self.paged_bytes = self.paged_bytes.saturating_add(size);
            self.cur_paged = self.cur_paged.saturating_add(1);
            if self.cur_paged > self.peak_paged {
                self.peak_paged = self.cur_paged;
            }
            if self.paged_bytes > self.peak_paged_bytes {
                self.peak_paged_bytes = self.paged_bytes;
            }
        } else {
            self.nonpaged_bytes = self.nonpaged_bytes.saturating_add(size);
            self.cur_nonpaged = self.cur_nonpaged.saturating_add(1);
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
        if let Some(i) = self.find_used(block) {
            let paged = self.entries[i].paged != 0;
            let size = self.entries[i].size;
            if paged {
                self.cur_paged = self.cur_paged.saturating_sub(1);
                self.paged_bytes = self.paged_bytes.saturating_sub(size);
            } else {
                self.cur_nonpaged = self.cur_nonpaged.saturating_sub(1);
                self.nonpaged_bytes = self.nonpaged_bytes.saturating_sub(size);
            }
            self.entries[i] = TrackEntry::default();
        }
    }

    /// Is `block` a tracked paged allocation?
    pub fn is_tracked_paged(&self, block: usize) -> bool {
        self.find_used(block).map(|i| self.entries[i].paged != 0).unwrap_or(false)
    }

    /// `VfCheckPoolAtUnload`: count entries whose caller lies in `[start, end)`. O(n) unload sweep,
    /// as in the C.
    pub fn leak_count(&self, start: usize, end: usize) -> u32 {
        let mut n = 0u32;
        for e in self.entries.iter() {
            if e.used != 0 && e.caller >= start && e.caller < end {
                n = n.saturating_add(1);
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
    // from_raw_parts requires a non-null pointer even for len 0; use an empty slice when null/empty.
    let slice: &mut [TrackEntry] = if entries.is_null() || cap == 0 {
        &mut []
    } else {
        unsafe { core::slice::from_raw_parts_mut(entries, cap) }
    };
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
pub unsafe extern "C" fn lrm_vf_track(st: *mut VfTrackState, entries: *mut TrackEntry, cap: usize, block: usize, size: u32, paged: u8, caller: usize, tag: u32) {
    if st.is_null() {
        return;
    }
    unsafe { with_tracker(st, entries, cap, |t| t.track(block, size, paged != 0, caller, tag)) }
}

/// `VfUntrackPool` over the C ABI.
/// # Safety
/// `st` and `entries` (for `cap` entries) must be valid and writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_untrack(st: *mut VfTrackState, entries: *mut TrackEntry, cap: usize, block: usize) {
    if st.is_null() {
        return;
    }
    unsafe { with_tracker(st, entries, cap, |t| t.untrack(block)) }
}

/// `VfpIsTrackedPaged` over the C ABI.
/// # Safety
/// `entries` must be valid for `cap` entries.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_is_tracked_paged(entries: *const TrackEntry, cap: usize, block: usize) -> u8 {
    if entries.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: non-null, cap entries (checked above).
    let slice = unsafe { core::slice::from_raw_parts(entries, cap) };
    // Hash-indexed lookup (open addressing, full-ring fallback), matching PoolTracker::find_used.
    let h = ((block >> 12) ^ (block >> 4)) % cap;
    for k in 0..cap {
        let e = &slice[(h + k) % cap];
        if e.used != 0 && e.block == block {
            return (e.paged != 0) as u8;
        }
    }
    0
}

/// `VfCheckPoolAtUnload` over the C ABI: 1 and `*out` set if a leak bug check is due, else 0.
/// # Safety
/// `entries` (for `cap`) and `out` must be valid.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_at_unload(entries: *const TrackEntry, cap: usize, name: usize, start: usize, end: usize, out: *mut BugCheck) -> u8 {
    if entries.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: non-null, cap entries (checked above).
    let slice = unsafe { core::slice::from_raw_parts(entries, cap) };
    let mut leaked = 0u32;
    for e in slice.iter() {
        if e.used != 0 && e.caller >= start && e.caller < end {
            leaked = leaked.saturating_add(1);
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

// ======================================================================================
// Stage 1b — Driver Verifier APIs (mirror of ntoskrnl/nvs/nt/ntdrvmgmt.c @ 5f5f92ed47a).
// The four APIs below are byte-identical at the later head; the level mask broadened there
// (adds IO/DEADLOCK), which is deferred with those unstable stages. Our code.
// ======================================================================================

// NTSTATUS values (public), as u32 bit patterns.
pub const STATUS_SUCCESS: u32 = 0x0000_0000;
const STATUS_NOT_SUPPORTED: u32 = 0xC000_00BB;
const STATUS_INVALID_PARAMETER_1: u32 = 0xC000_00EF;
const STATUS_INVALID_PARAMETER_2: u32 = 0xC000_00F0;

// Verifier level bits (public Driver Verifier flags) and the implemented set at the pinned SHA.
const DRIVER_VERIFIER_SPECIAL_POOLING: u32 = 0x1;
const DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS: u32 = 0x8;
const MI_VERIFIER_IMPLEMENTED_LEVEL: u32 = DRIVER_VERIFIER_SPECIAL_POOLING | DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS; // 0x9

// Internal LDR flag (fed identically to both sides in the differential).
const LDRP_IMAGE_VERIFYING: u32 = 0x0002_0000;

// ASCII separators / wildcard / case-fold, as UTF-16 code units.
const SEP_SPACE: u16 = 0x20;
const SEP_TAB: u16 = 0x09;
const SEP_COMMA: u16 = 0x2C;
const SEP_SEMI: u16 = 0x3B;
const WILDCARD: u16 = 0x2A; // '*'
const UC_A: u16 = 0x41; // 'A'
const UC_Z: u16 = 0x5A; // 'Z'

/// `MmIsVerifierEnabled`: active -> (level, STATUS_SUCCESS); else (0, STATUS_NOT_SUPPORTED).
pub fn is_verifier_enabled(active: bool, level: u32) -> (u32, u32) {
    if active {
        (level, STATUS_SUCCESS)
    } else {
        (0, STATUS_NOT_SUPPORTED)
    }
}

/// `MmIsDriverVerifying`: no LDR section -> false; else the `LDRP_IMAGE_VERIFYING` flag.
pub fn is_driver_verifying(ldr_present: bool, flags: u32) -> bool {
    ldr_present && (flags & LDRP_IMAGE_VERIFYING) != 0
}

/// `MmAddVerifierThunks` validation -> NTSTATUS. `sizeof_pair` is `sizeof(DRIVER_VERIFIER_THUNK_PAIRS)`;
/// `module_found`/`module_base`/`module_end` describe the module `PristineRoutine` resolves to;
/// `loaded_imports_invalid` is `LoadedImports == (PVOID)-1`; `pristines` are the thunk PristineRoutines.
pub fn add_verifier_thunks(
    list_init: bool,
    buf_size: usize,
    sizeof_pair: usize,
    module_found: bool,
    module_base: usize,
    module_end: usize,
    loaded_imports_invalid: bool,
    pristines: &[usize],
) -> u32 {
    if !list_init {
        return STATUS_NOT_SUPPORTED;
    }
    let count = if sizeof_pair == 0 { 0 } else { buf_size / sizeof_pair };
    if count == 0 {
        return STATUS_INVALID_PARAMETER_1;
    }
    if !module_found {
        return STATUS_INVALID_PARAMETER_2;
    }
    if loaded_imports_invalid {
        return STATUS_INVALID_PARAMETER_2;
    }
    for &p in pristines.iter().take(count) {
        if p < module_base || p >= module_end {
            return STATUS_INVALID_PARAMETER_2;
        }
    }
    STATUS_SUCCESS
}

/// `MiInitializeDriverVerifier` level computation: `None` = verifier not enabled (no active state);
/// `Some(level)` = the level recorded (and the verifier becomes active). `verify_driver_level` is
/// `MmVerifyDriverLevel` (`0xFFFFFFFF` = unset -> the implemented default).
pub fn init_level(verify_driver_level: u32, special_pool_enabled: bool, buffer_empty: bool) -> Option<u32> {
    if buffer_empty {
        return None;
    }
    let mut level = if verify_driver_level == 0xFFFF_FFFF {
        MI_VERIFIER_IMPLEMENTED_LEVEL
    } else {
        verify_driver_level
    };
    level &= MI_VERIFIER_IMPLEMENTED_LEVEL;
    if !special_pool_enabled {
        level &= !DRIVER_VERIFIER_SPECIAL_POOLING;
    }
    if level == 0 {
        return None;
    }
    Some(level)
}

#[inline]
fn is_name_sep(c: u16) -> bool {
    c == SEP_SPACE || c == SEP_TAB || c == SEP_COMMA || c == SEP_SEMI
}

/// Case fold for the name match. ASCII A-Z only. The C uses `RtlEqualUnicodeString(CaseInsensitive)`
/// = `RtlUpcaseUnicodeChar` over the full BMP; for the ASCII driver names that `VerifyDrivers` carries
/// in practice the two agree (A-Z vs a-z). Full Unicode case folding is deferred; if a non-ASCII name
/// ever needs matching, replace this with the upcase-table equivalent.
#[inline]
fn ascii_fold(c: u16) -> u16 {
    if (UC_A..=UC_Z).contains(&c) {
        c + 32
    } else {
        c
    }
}

/// `MiIsVerifiedDriverName`: is `name` in the space/tab/comma/semicolon-separated `buffer`
/// (UTF-16), or does the buffer contain the single `*`? Name match is case-insensitive (ASCII).
/// (The C is `static`, reached only via `MiVerifyLoadedImage`; host-unit-tested, not differential.)
pub fn is_verified_driver_name(buffer: &[u16], name: &[u16]) -> bool {
    let mut i = 0usize;
    loop {
        while i < buffer.len() && buffer[i] != 0 && is_name_sep(buffer[i]) {
            i += 1;
        }
        if i >= buffer.len() || buffer[i] == 0 {
            return false;
        }
        let start = i;
        while i < buffer.len() && buffer[i] != 0 && !is_name_sep(buffer[i]) {
            i += 1;
        }
        let Some(tok) = buffer.get(start..i) else {
            return false;
        };
        if tok.len() == 1 && tok[0] == WILDCARD {
            return true;
        }
        if tok.len() == name.len() && tok.iter().zip(name.iter()).all(|(&a, &b)| ascii_fold(a) == ascii_fold(b)) {
            return true;
        }
    }
}

// ---- C ABI for the Stage 1b differential ----

/// `MmIsVerifierEnabled` classifier: writes flags to `*out_flags`, returns the NTSTATUS (u32).
/// # Safety
/// `out_flags` must be writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_is_verifier_enabled(active: u8, level: u32, out_flags: *mut u32) -> u32 {
    let (f, s) = is_verifier_enabled(active != 0, level);
    if !out_flags.is_null() {
        unsafe { *out_flags = f };
    }
    s
}

/// `MmIsDriverVerifying`.
#[no_mangle]
pub extern "C" fn lrm_vf_is_driver_verifying(ldr_present: u8, flags: u32) -> u8 {
    is_driver_verifying(ldr_present != 0, flags) as u8
}

/// `MmAddVerifierThunks` validation -> NTSTATUS (u32).
/// # Safety
/// `pristines` must point to `n` valid `usize` values (may be null if `n == 0`).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lrm_vf_add_verifier_thunks(
    list_init: u8,
    buf_size: usize,
    sizeof_pair: usize,
    module_found: u8,
    module_base: usize,
    module_end: usize,
    loaded_imports_invalid: u8,
    pristines: *const usize,
    n: usize,
) -> u32 {
    let slice = if pristines.is_null() || n == 0 {
        &[][..]
    } else {
        // SAFETY: caller guarantees `pristines` valid for `n`.
        unsafe { core::slice::from_raw_parts(pristines, n) }
    };
    add_verifier_thunks(list_init != 0, buf_size, sizeof_pair, module_found != 0, module_base, module_end, loaded_imports_invalid != 0, slice)
}

/// `MiInitializeDriverVerifier` level computation: returns 1 and writes the level to `*out_level`
/// when the verifier becomes active, else 0.
/// # Safety
/// `out_level` must be writable.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_init_level(verify_driver_level: u32, special_pool_enabled: u8, buffer_empty: u8, out_level: *mut u32) -> u8 {
    match init_level(verify_driver_level, special_pool_enabled != 0, buffer_empty != 0) {
        Some(l) => {
            if !out_level.is_null() {
                unsafe { *out_level = l };
            }
            1
        }
        None => 0,
    }
}

/// `MiIsVerifiedDriverName` (host unit helper; C is static).
/// # Safety
/// `buffer`/`name` must point to `blen`/`nlen` valid u16 values.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_is_verified_driver_name(buffer: *const u16, blen: usize, name: *const u16, nlen: usize) -> u8 {
    // from_raw_parts needs a non-null pointer even for len 0; treat null/empty as an empty slice.
    let b: &[u16] = if buffer.is_null() || blen == 0 {
        &[]
    } else {
        // SAFETY: non-null, blen u16 (checked above).
        unsafe { core::slice::from_raw_parts(buffer, blen) }
    };
    let n: &[u16] = if name.is_null() || nlen == 0 {
        &[]
    } else {
        // SAFETY: non-null, nlen u16 (checked above).
        unsafe { core::slice::from_raw_parts(name, nlen) }
    };
    is_verified_driver_name(b, n) as u8
}

// ======================================================================================
// Stage 3 — I/O verification rules (mirror of `ntoskrnl/vf/vfio.c`), bug check 0xC9
// ======================================================================================
//
// Clean-room Rust of the I/O-manager verification decisions the C enforces at a verified driver's
// IRP calls (`VfIoThunks[]`). Hard rules always bug-check `DRIVER_VERIFIER_IOMANAGER_VIOLATION`
// (0xC9) with the sub-code in P1. The soft (table) rules route through `VfpReportIrpRule`, whose
// severity is: count the hit, then — if the rule is fatal AND trusted — bug-check with NO log line,
// else print one log line (first `VF_IRP_LINES_PRINTED`). The IRP-table rules 0x205/0x209/0x20B/0x310
// fire only for a *pended* IRP entry (`VfpIsIrpBelow`), and are *withheld* (never fatal, log only)
// once `VfIrpUntrackSkipped != 0`, even when the rule is otherwise fatal. The `caller` parameter is a
// return address and MAY be a kernel address (driver tail-call), so no in-driver-range assumption.

/// `DRIVER_VERIFIER_IOMANAGER_VIOLATION` (public bug-check code).
pub const BUGCHECK_DRIVER_VERIFIER_IOMANAGER: u32 = 0xC9;

// Hard I/O sub-codes (param1 of 0xC9), exactly as vfio.c raises them.
const VF_IO_FREE_NOT_AN_IRP: u32 = 0x01;
const VF_IO_FREE_QUEUED_TO_THREAD: u32 = 0x02;
const VF_IO_CALL_NOT_AN_IRP: u32 = 0x03;
const VF_IO_CALL_BAD_DEVICE: u32 = 0x04;
const VF_IO_CALL_IRQL_CHANGED: u32 = 0x05;
const VF_IO_COMPLETE_PENDING_STATUS: u32 = 0x06;
const VF_IO_COMPLETE_CANCEL_ROUTINE: u32 = 0x07;
const VF_IO_COMPLETE_IRQL: u32 = 0x0E;
const VF_IO_CALL_IRQL: u32 = 0x10;

// Soft (table) I/O sub-codes.
const VF_IO_CALL_CANCEL_ROUTINE: u32 = 0x203;
const VF_IO_CALL_QUEUED_BELOW: u32 = 0x205;
const VF_IO_COMPLETE_QUEUED_BELOW: u32 = 0x209;
const VF_IO_FREE_IN_USE: u32 = 0x20B;
const VF_IO_REINITIALIZE_IN_USE: u32 = 0x310;

/// Max log-only lines printed by the I/O report path (the shared `VfIrpRuleHits` cap).
const VF_IRP_LINES_PRINTED: u32 = 64;

/// `STATUS_PENDING` — a completion with this (or -1) status is a hard 0x06 violation.
const STATUS_PENDING: i32 = 0x0000_0103u32 as i32;

/// The observable result of a verifier check that may be fatal, logged, or silent.
///
/// `Log` carries the rule sub-code (the line the C would print); `Fatal` carries the exact
/// `BugCheck` the C would raise. Hard checks only ever produce `Ok`/`Fatal`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum IoOutcome {
    /// No violation observed.
    Ok,
    /// Counted and logged (one "(log only)" line), not fatal.
    Log(u32),
    /// Fatal — the kernel shim raises this bug check.
    Fatal(BugCheck),
}

/// `KeBugCheckEx(0xC9, SubCode, A, B, C)` — the I/O-manager parameter layout (sub-code in P1).
#[inline]
fn ioviol(sub: u32, a: usize, b: usize, c: usize) -> BugCheck {
    BugCheck { code: BUGCHECK_DRIVER_VERIFIER_IOMANAGER, p1: sub as usize, p2: a, p3: b, p4: c }
}

/// `VfpCheckCall`: the hard pre-send checks on `IoCallDriver`.
/// `irql > DISPATCH` -> 0x10; `!irp || Type != IO_TYPE_IRP` -> 0x03; `!dev || Type != IO_TYPE_DEVICE`
/// -> 0x04 (first failing wins, matching the C order).
pub fn vfio_check_call(irql: u8, irp: usize, irp_is_irp: bool, dev: usize, dev_is_dev: bool) -> Result<(), BugCheck> {
    if irql > DISPATCH_LEVEL {
        return Err(ioviol(VF_IO_CALL_IRQL, irql as usize, 0, 0));
    }
    if irp == 0 || !irp_is_irp {
        return Err(ioviol(VF_IO_CALL_NOT_AN_IRP, irp, 0, 0));
    }
    if dev == 0 || !dev_is_dev {
        return Err(ioviol(VF_IO_CALL_BAD_DEVICE, dev, 0, 0));
    }
    Ok(())
}

/// `VfpCheckCallReturn`: IRQL must be unchanged across the driver call. `after != before` -> 0x05
/// `(dev, before, after)`.
pub fn vfio_check_call_return(dev: usize, before: u8, after: u8) -> Result<(), BugCheck> {
    if after != before {
        return Err(ioviol(VF_IO_CALL_IRQL_CHANGED, dev, before as usize, after as usize));
    }
    Ok(())
}

/// `VfpCheckComplete`: the hard checks on `IoCompleteRequest`.
/// `irql > DISPATCH` -> 0x0E `(irql, irp, 0)`; status is STATUS_PENDING or -1 -> 0x06
/// `((ULONG_PTR)status, irp, 0)`; a cancel routine is set -> 0x07 `(cancel, irp, 0)`.
pub fn vfio_check_complete(irql: u8, irp: usize, status: i32, cancel: usize) -> Result<(), BugCheck> {
    if irql > DISPATCH_LEVEL {
        return Err(ioviol(VF_IO_COMPLETE_IRQL, irql as usize, irp, 0));
    }
    if status == STATUS_PENDING || status == -1 {
        // C passes (ULONG_PTR)Irp->IoStatus.Status: a signed 32-bit NTSTATUS widened to the pointer
        // width (so -1 becomes all-ones). `as isize as usize` reproduces that sign-extension.
        return Err(ioviol(VF_IO_COMPLETE_PENDING_STATUS, status as isize as usize, irp, 0));
    }
    if cancel != 0 {
        return Err(ioviol(VF_IO_COMPLETE_CANCEL_ROUTINE, cancel, irp, 0));
    }
    Ok(())
}

/// `VfIoFreeIrp`: hard checks on `IoFreeIrp`.
/// `Type != IO_TYPE_IRP` -> 0x01; its thread list is non-empty -> 0x02.
pub fn vfio_free_irp(irp: usize, is_irp: bool, thread_list_empty: bool) -> Result<(), BugCheck> {
    if !is_irp {
        return Err(ioviol(VF_IO_FREE_NOT_AN_IRP, irp, 0, 0));
    }
    if !thread_list_empty {
        return Err(ioviol(VF_IO_FREE_QUEUED_TO_THREAD, irp, 0, 0));
    }
    Ok(())
}

/// `VfpIsIrpRuleFatal`: only 0x203 and 0x20B are fatal among the soft rules.
pub fn vfio_rule_fatal(rule: u32) -> bool {
    matches!(rule, VF_IO_CALL_CANCEL_ROUTINE | VF_IO_FREE_IN_USE)
}

/// `VfpIsIrpTableTrusted`: the IRP-table rules (0x205/0x209/0x20B/0x310) are trusted only while
/// `VfIrpUntrackSkipped == 0`; every other rule is always trusted. An untrusted rule is never fatal.
pub fn vfio_table_trusted(rule: u32, untrack_skipped: u32) -> bool {
    match rule {
        VF_IO_CALL_QUEUED_BELOW | VF_IO_COMPLETE_QUEUED_BELOW | VF_IO_FREE_IN_USE | VF_IO_REINITIALIZE_IN_USE => {
            untrack_skipped == 0
        }
        _ => true,
    }
}

/// `VfpReportIrpRule`: count the hit, then decide. Order is exact: increment `*hits`; if the rule is
/// fatal AND trusted -> `Fatal` (no log line); else if `*hits <= VF_IRP_LINES_PRINTED` -> `Log`; else
/// `Ok` (counted but suppressed). `hits` mirrors the kernel's shared `VfIrpRuleHits`.
pub fn vfio_report(rule: u32, caller: usize, irp: usize, untrack_skipped: u32, hits: &mut u32) -> IoOutcome {
    *hits = hits.saturating_add(1);
    if vfio_rule_fatal(rule) && vfio_table_trusted(rule, untrack_skipped) {
        return IoOutcome::Fatal(ioviol(rule, caller, irp, 0));
    }
    if *hits <= VF_IRP_LINES_PRINTED {
        IoOutcome::Log(rule)
    } else {
        IoOutcome::Ok
    }
}

/// Whether a soft I/O rule's *call-site condition* holds (so `VfpReportIrpRule` is even invoked).
/// 0x203 fires when a cancel routine is set (no pended gate). 0x205/0x209 fire when the IRP is pended
/// AND the caller does not own the current stack location. 0x20B/0x310 fire when the IRP is pended.
pub fn vfio_soft_condition(rule: u32, cancel_present: bool, pended: bool, caller_owns: bool) -> bool {
    match rule {
        VF_IO_CALL_CANCEL_ROUTINE => cancel_present,
        VF_IO_CALL_QUEUED_BELOW | VF_IO_COMPLETE_QUEUED_BELOW => pended && !caller_owns,
        VF_IO_FREE_IN_USE | VF_IO_REINITIALIZE_IN_USE => pended,
        _ => false,
    }
}

/// Full soft-rule decision at a call site: if the condition does not hold, `Ok` with NO hit counted
/// (matching the C, which never calls the report path); otherwise the `vfio_report` outcome.
#[allow(clippy::too_many_arguments)]
pub fn vfio_soft_eval(
    rule: u32,
    cancel_present: bool,
    pended: bool,
    caller_owns: bool,
    caller: usize,
    irp: usize,
    untrack_skipped: u32,
    hits: &mut u32,
) -> IoOutcome {
    if !vfio_soft_condition(rule, cancel_present, pended, caller_owns) {
        return IoOutcome::Ok;
    }
    vfio_report(rule, caller, irp, untrack_skipped, hits)
}

// ======================================================================================
// Stage 4 — deadlock / lock-order rules (mirror of `ntoskrnl/vf/vfdeadlock.c`), bug check 0xC4
// ======================================================================================
//
// Clean-room Rust of the lock-order verification severity the C enforces (`VfpReportDeadlockRule`).
// Only WRONG_THREAD (0x1004) and DELETED_OWNED (0x100B) are fatal; both carry three extra params
// `(lock, second, third)`. The remaining rules (SELF 0x1000, HIERARCHY 0x1001, UNACQUIRED 0x1007)
// are log-only (first `VF_LOCK_LINES_PRINTED`). The graph-walk/held-record bookkeeping stays in C
// (it owns the graph state); the Rust port is the observable severity decision the differential
// drives through the exported lock hooks.

const VF_DEADLOCK_SELF: u32 = 0x1000;
const VF_DEADLOCK_HIERARCHY: u32 = 0x1001;
const VF_DEADLOCK_WRONG_THREAD: u32 = 0x1004;
const VF_DEADLOCK_UNACQUIRED: u32 = 0x1007;
const VF_DEADLOCK_DELETED_OWNED: u32 = 0x100B;

/// Max log-only lines printed by the deadlock report path (the shared `VfDeadlockRuleHits` cap).
const VF_LOCK_LINES_PRINTED: u32 = 64;

/// `VfpIsDeadlockRuleFatal`: only WRONG_THREAD and DELETED_OWNED are fatal (all five rules listed
/// explicitly, mirroring the C switch).
pub fn vfdl_rule_fatal(rule: u32) -> bool {
    match rule {
        VF_DEADLOCK_SELF => false,
        VF_DEADLOCK_HIERARCHY => false,
        VF_DEADLOCK_WRONG_THREAD => true,
        VF_DEADLOCK_UNACQUIRED => false,
        VF_DEADLOCK_DELETED_OWNED => true,
        _ => false,
    }
}

/// `VfpReportDeadlockRule`: count the hit, then decide. If fatal -> `0xC4 (rule, lock, second, third)`
/// for WRONG_THREAD/DELETED_OWNED, else `0xC4 (rule, lock, 0, 0)` (the latter is a defensive branch:
/// no other rule is fatal today). Non-fatal -> `Log` while `*hits <= VF_LOCK_LINES_PRINTED`, else `Ok`.
pub fn vfdl_report(rule: u32, lock: usize, second: usize, third: usize, hits: &mut u32) -> IoOutcome {
    *hits = hits.saturating_add(1);
    if vfdl_rule_fatal(rule) {
        if rule == VF_DEADLOCK_WRONG_THREAD || rule == VF_DEADLOCK_DELETED_OWNED {
            return IoOutcome::Fatal(BugCheck {
                code: BUGCHECK_DRIVER_VERIFIER,
                p1: rule as usize,
                p2: lock,
                p3: second,
                p4: third,
            });
        }
        return IoOutcome::Fatal(BugCheck { code: BUGCHECK_DRIVER_VERIFIER, p1: rule as usize, p2: lock, p3: 0, p4: 0 });
    }
    if *hits <= VF_LOCK_LINES_PRINTED {
        IoOutcome::Log(rule)
    } else {
        IoOutcome::Ok
    }
}

// ----- C ABI for the stage 3/4 ports (host differential + in-kernel shim) -----

/// Map an `IoOutcome` to the ABI discriminant (0 = Ok, 1 = Log, 2 = Fatal), writing the rule to
/// `*out_rule` for `Log` and the bug check to `*out_bc` for `Fatal`.
///
/// # Safety
/// `out_bc`/`out_rule` must be writable or null.
#[inline]
unsafe fn io_emit(o: IoOutcome, out_bc: *mut BugCheck, out_rule: *mut u32) -> u8 {
    match o {
        IoOutcome::Ok => 0,
        IoOutcome::Log(r) => {
            if !out_rule.is_null() {
                unsafe { *out_rule = r };
            }
            1
        }
        IoOutcome::Fatal(bc) => {
            if !out_bc.is_null() {
                unsafe { *out_bc = bc };
            }
            2
        }
    }
}

/// `VfpCheckCall` -> 1 and writes `*out` when it bug-checks, else 0.
/// # Safety
/// `out` must be writable or null.
#[no_mangle]
pub unsafe extern "C" fn lrm_vfio_check_call(irql: u8, irp: usize, irp_is_irp: u8, dev: usize, dev_is_dev: u8, out: *mut BugCheck) -> u8 {
    match vfio_check_call(irql, irp, irp_is_irp != 0, dev, dev_is_dev != 0) {
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
        Ok(()) => 0,
    }
}

/// `VfpCheckCallReturn` -> 1 and writes `*out` when it bug-checks, else 0.
/// # Safety
/// `out` must be writable or null.
#[no_mangle]
pub unsafe extern "C" fn lrm_vfio_check_call_return(dev: usize, before: u8, after: u8, out: *mut BugCheck) -> u8 {
    match vfio_check_call_return(dev, before, after) {
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
        Ok(()) => 0,
    }
}

/// `VfpCheckComplete` hard checks -> 1 and writes `*out` when it bug-checks, else 0.
/// # Safety
/// `out` must be writable or null.
#[no_mangle]
pub unsafe extern "C" fn lrm_vfio_check_complete(irql: u8, irp: usize, status: i32, cancel: usize, out: *mut BugCheck) -> u8 {
    match vfio_check_complete(irql, irp, status, cancel) {
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
        Ok(()) => 0,
    }
}

/// `VfIoFreeIrp` hard checks -> 1 and writes `*out` when it bug-checks, else 0.
/// # Safety
/// `out` must be writable or null.
#[no_mangle]
pub unsafe extern "C" fn lrm_vfio_free_irp(irp: usize, is_irp: u8, thread_list_empty: u8, out: *mut BugCheck) -> u8 {
    match vfio_free_irp(irp, is_irp != 0, thread_list_empty != 0) {
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
        Ok(()) => 0,
    }
}

/// `VfpIsIrpRuleFatal`.
#[no_mangle]
pub extern "C" fn lrm_vfio_rule_fatal(rule: u32) -> u8 {
    vfio_rule_fatal(rule) as u8
}

/// `VfpIsIrpTableTrusted`.
#[no_mangle]
pub extern "C" fn lrm_vfio_table_trusted(rule: u32, untrack_skipped: u32) -> u8 {
    vfio_table_trusted(rule, untrack_skipped) as u8
}

/// Full soft I/O rule decision at a call site (`vfio_soft_eval`). Returns 0 = Ok, 1 = Log, 2 = Fatal;
/// writes the rule to `*out_rule` for Log and the bug check to `*out_bc` for Fatal; advances `*hits`.
/// # Safety
/// `hits`/`out_bc`/`out_rule` must be writable or null.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lrm_vfio_soft_eval(
    rule: u32,
    cancel_present: u8,
    pended: u8,
    caller_owns: u8,
    caller: usize,
    irp: usize,
    untrack_skipped: u32,
    hits: *mut u32,
    out_bc: *mut BugCheck,
    out_rule: *mut u32,
) -> u8 {
    let mut local = if hits.is_null() { 0 } else { unsafe { *hits } };
    let o = vfio_soft_eval(rule, cancel_present != 0, pended != 0, caller_owns != 0, caller, irp, untrack_skipped, &mut local);
    if !hits.is_null() {
        unsafe { *hits = local };
    }
    unsafe { io_emit(o, out_bc, out_rule) }
}

/// `VfpIsDeadlockRuleFatal`.
#[no_mangle]
pub extern "C" fn lrm_vfdl_rule_fatal(rule: u32) -> u8 {
    vfdl_rule_fatal(rule) as u8
}

/// `VfpReportDeadlockRule`. Returns 0 = Ok, 1 = Log, 2 = Fatal; writes the rule to `*out_rule` for
/// Log and the bug check to `*out_bc` for Fatal; advances `*hits`.
/// # Safety
/// `hits`/`out_bc`/`out_rule` must be writable or null.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lrm_vfdl_report(
    rule: u32,
    lock: usize,
    second: usize,
    third: usize,
    hits: *mut u32,
    out_bc: *mut BugCheck,
    out_rule: *mut u32,
) -> u8 {
    let mut local = if hits.is_null() { 0 } else { unsafe { *hits } };
    let o = vfdl_report(rule, lock, second, third, &mut local);
    if !hits.is_null() {
        unsafe { *hits = local };
    }
    unsafe { io_emit(o, out_bc, out_rule) }
}
