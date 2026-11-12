//! `SpinLock<T>` as RAII over an NT `KSPIN_LOCK` (v2).
//!
//! Owner-approved v2 scope: a `SpinLock<T>` that acquires/releases the NT spin lock with
//! the IRQL raised to DISPATCH_LEVEL on acquire and restored on release, where "release"
//! is guaranteed by `Drop` on every exit from the guarded scope.
//!
//! On this crate `panic = "abort"`, so there is no stack unwinding: the only way to leave
//! a scope without running the guard's `Drop` is the controlled bugcheck, which tears the
//! box down anyway (IRQL is then moot). On every ordinary path — fallthrough, early
//! `return`, `?` — `Drop` runs and the IRQL is restored. That is the RAII guarantee the
//! owner asked for, including what would be the unwind path on an unwinding target.

use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};

use crate::ndk::{KIRQL, KSPIN_LOCK};

// ntoskrnl spin-lock imports (resolved at driver link). On x64 the `KeAcquireSpinLock`
// macro expands to `*OldIrql = KeAcquireSpinLockRaiseToDpc(SpinLock)`, so we bind that
// exported function directly. Names/arities match ntoskrnl.spec.
extern "C" {
    fn KeInitializeSpinLock(spin_lock: *mut KSPIN_LOCK);
    fn KeAcquireSpinLockRaiseToDpc(spin_lock: *mut KSPIN_LOCK) -> KIRQL;
    fn KeReleaseSpinLock(spin_lock: *mut KSPIN_LOCK, new_irql: KIRQL);
}

/// A mutual-exclusion lock over `T`, backed by an NT `KSPIN_LOCK`. Acquire raises IRQL to
/// DISPATCH_LEVEL; the guard restores it on drop. `T` must be `Send` to cross the lock.
pub struct SpinLock<T> {
    lock: UnsafeCell<KSPIN_LOCK>,
    data: UnsafeCell<T>,
}

// A spin lock provides the synchronization that makes shared access sound.
unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// Create a lock. The `KSPIN_LOCK` starts zeroed, which is the exact state
    /// `KeInitializeSpinLock` establishes (unowned), so this is valid in `static`s.
    pub const fn new(data: T) -> Self {
        SpinLock {
            lock: UnsafeCell::new(0),
            data: UnsafeCell::new(data),
        }
    }

    /// Explicitly run `KeInitializeSpinLock` over the storage. Optional given `new`, but
    /// exercises the real init export and documents intent.
    pub fn init(&self) {
        // SAFETY: self.lock is a live, properly aligned KSPIN_LOCK slot.
        unsafe { KeInitializeSpinLock(self.lock.get()) };
    }

    /// Acquire the lock, raising IRQL to DISPATCH_LEVEL, and return an RAII guard. The
    /// IRQL is restored when the guard drops.
    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        // SAFETY: valid KSPIN_LOCK; returns the caller's prior IRQL to restore on drop.
        let old_irql = unsafe { KeAcquireSpinLockRaiseToDpc(self.lock.get()) };
        SpinLockGuard { parent: self, irql: old_irql }
    }
}

/// RAII guard. Deref gives access to the protected data; `Drop` releases the lock and
/// restores the acquiring thread's IRQL.
pub struct SpinLockGuard<'a, T> {
    parent: &'a SpinLock<T>,
    irql: KIRQL,
}

impl<T> Deref for SpinLockGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the lock is held for the guard's lifetime, so shared access is sound.
        unsafe { &*self.parent.data.get() }
    }
}

impl<T> DerefMut for SpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: the lock is held and the guard is uniquely borrowed here.
        unsafe { &mut *self.parent.data.get() }
    }
}

impl<T> Drop for SpinLockGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: we hold the lock; release it and restore the saved IRQL. This runs on
        // every scope exit (RAII), which is the IRQL-restore guarantee required by v2.
        unsafe { KeReleaseSpinLock(self.parent.lock.get(), self.irql) };
    }
}
