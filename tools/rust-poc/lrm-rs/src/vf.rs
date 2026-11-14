//! LRM-RS — Driver Verifier contract stages (clean-room Rust).
//!
//! Clean-room Rust of the Driver-Verifier decision logic that the C enforces on a *verified*
//! driver (Dwight's `ntoskrnl/vf/vfirql.c`, `vfpool.c` and `nvs/nt/ntdrvmgmt.c`, observed via
//! Ryan's `OBSERVED-CONTRACTS-SPECIAL-POOL-VERIFIER.md` and the C reference pinned at
//! `feat/special-pool 5f5f92ed47a`). OUR code — contracts observed, no source copied (owner rule).
//!
//! This module holds the *pure decision* cores: given the inputs the kernel has already gathered
//! (current IRQL, pool type, sizes, tracking state), decide whether to proceed or which exact
//! `DRIVER_VERIFIER_DETECTED_VIOLATION` (0xC4) bug check to raise, with the precise sub-code and
//! parameter positions. The bug check is returned as a `BugCheck` value; the kernel shim raises
//! `KeBugCheckEx`, the host harness asserts it against the C stage code.
//!
//! Stage 2a (this commit): IRQL rules at the lock/wait calls of a verified driver.

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

// ---- Stage 2a: IRQL rules (mirror of vfirql.c) ----

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

/// `ExReleaseFastMutex`: must be exactly AT APC. `Irql != APC` -> 0x34; note P3 is the thread's
/// `CombinedApcDisable` and P4 is the mutex (the C's parameter order).
pub fn release_fast_mutex(irql: u8, mutex: usize, combined_apc_disable: usize) -> Result<(), BugCheck> {
    if irql != APC_LEVEL {
        return Err(viol(VF_RELEASE_FAST_MUTEX_IRQL, irql, combined_apc_disable, mutex));
    }
    Ok(())
}

/// `KeWaitForSingleObject`/`KeWaitForMultipleObjects`: a wait below DISPATCH is fine; a
/// zero-timeout poll at DISPATCH is fine; anything else at or above DISPATCH -> 0x3B. `timeout` is
/// the caller's `PLARGE_INTEGER` (0 = NULL); `timeout_zero` is `Timeout->QuadPart == 0`.
pub fn check_wait(irql: u8, object: usize, timeout: usize, timeout_zero: bool) -> Result<(), BugCheck> {
    if irql < DISPATCH_LEVEL {
        return Ok(());
    }
    if irql == DISPATCH_LEVEL && timeout != 0 && timeout_zero {
        return Ok(());
    }
    Err(viol(VF_WAIT_AT_DISPATCH, irql, object, timeout))
}

// ---- C ABI for the host differential ----

macro_rules! abi1 {
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

abi1!(lrm_vf_acquire_spinlock_at_dpc, acquire_spinlock_at_dpc);
abi1!(lrm_vf_release_spinlock_from_dpc, release_spinlock_from_dpc);
abi1!(lrm_vf_acquire_spinlock_raise_to_dpc, acquire_spinlock_raise_to_dpc);
abi1!(lrm_vf_release_spinlock, release_spinlock);
abi1!(lrm_vf_acquire_fast_mutex, acquire_fast_mutex);

/// `ExReleaseFastMutex` classifier over the C ABI.
///
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_release_fast_mutex(
    irql: u8,
    mutex: usize,
    combined_apc_disable: usize,
    out: *mut BugCheck,
) -> u8 {
    match release_fast_mutex(irql, mutex, combined_apc_disable) {
        Ok(()) => 0,
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
    }
}

/// Wait classifier over the C ABI.
///
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_vf_check_wait(
    irql: u8,
    object: usize,
    timeout: usize,
    timeout_zero: u8,
    out: *mut BugCheck,
) -> u8 {
    match check_wait(irql, object, timeout, timeout_zero != 0) {
        Ok(()) => 0,
        Err(bc) => {
            if !out.is_null() {
                unsafe { *out = bc };
            }
            1
        }
    }
}
