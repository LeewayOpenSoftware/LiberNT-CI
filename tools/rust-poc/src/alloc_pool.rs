//! Pool-backed `#[global_allocator]` with fallible `try_*` APIs (v2).
//!
//! Owner-approved v2 scope: a `#[global_allocator]` backed by the NT nonpaged pool, and
//! a fallible allocation surface where an allocation failure returns `Err(AllocError)`
//! and NEVER panics or bugchecks. The registered global allocator follows the
//! `GlobalAlloc` contract (null on failure); the public `try_*` / `KBox` API checks that
//! null and turns it into an `Err`, so no caller ever reaches `handle_alloc_error`.
//!
//! No infallible `alloc` API (`Box::new`, `Vec::push`, ...) is used anywhere in this
//! crate, which is why the OOM path is always a returned error, never a trap.

use core::alloc::{GlobalAlloc, Layout};
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

use crate::ndk::SINGLE_LIST_ENTRY;

// ---- ntoskrnl pool imports (resolved at driver link) -------------------------------
// Signatures match ntoskrnl.spec (x64): ExAllocatePoolWithTag(POOL_TYPE, SIZE_T, ULONG),
// ExFreePoolWithTag(PVOID, ULONG). The plain WithTag form returns NULL on failure and
// does NOT raise, which is exactly what a fallible allocator needs.
extern "C" {
    fn ExAllocatePoolWithTag(pool_type: i32, bytes: usize, tag: u32) -> *mut u8;
    fn ExFreePoolWithTag(p: *mut u8, tag: u32);
}

/// `NonPagedPool` (POOL_TYPE == 0). Nonpaged so allocations are legal to touch at
/// IRQL <= DISPATCH_LEVEL (e.g. while a `SpinLock` is held).
const NON_PAGED_POOL: i32 = 0;

/// Pool tag, shown as "LrPc" (Leeway Rust PoC) in pool diagnostics.
const POOL_TAG: u32 = u32::from_le_bytes([b'L', b'r', b'P', b'c']);

/// NT nonpaged-pool allocations are guaranteed aligned to `MEMORY_ALLOCATION_ALIGNMENT`
/// (16 bytes on x64). Over-aligned requests are refused (null) rather than risking UB.
const POOL_GUARANTEED_ALIGN: usize = 16;

/// A pool-backed global allocator. One zero-sized unit; the pool itself is the state.
pub struct PoolAllocator;

unsafe impl GlobalAlloc for PoolAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // Honour the GlobalAlloc contract: a returned pointer must meet layout.align().
        // The pool guarantees 16-byte alignment; anything stricter is refused (null).
        if layout.align() > POOL_GUARANTEED_ALIGN {
            return core::ptr::null_mut();
        }
        if layout.size() == 0 {
            // Never hand a real allocation to a zero-size request.
            return core::ptr::null_mut();
        }
        // SAFETY: FFI to the NT pool allocator; returns NULL on failure (no raise).
        unsafe { ExAllocatePoolWithTag(NON_PAGED_POOL, layout.size(), POOL_TAG) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: ptr came from alloc() above, so it is a pool block with our tag.
        unsafe { ExFreePoolWithTag(ptr, POOL_TAG) }
    }
}

/// The registered process-wide global allocator. Present so `#[global_allocator]` is a
/// real pool allocator (owner requirement); the crate never drives it through an
/// infallible path.
#[global_allocator]
static GLOBAL: PoolAllocator = PoolAllocator;

/// The single error type for every fallible allocation in this crate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AllocError;

/// Fallible raw allocation. Routes through the registered `#[global_allocator]` and
/// turns the null-on-failure contract into `Err(AllocError)`. Never panics.
pub fn try_alloc(layout: Layout) -> Result<NonNull<u8>, AllocError> {
    if layout.size() == 0 {
        // A dangling, well-aligned pointer is the correct answer for a ZST request.
        return Ok(NonNull::new(layout.align() as *mut u8).unwrap_or(NonNull::dangling()));
    }
    // SAFETY: non-zero layout; alloc() returns null on failure, checked below.
    let raw = unsafe { GLOBAL.alloc(layout) };
    NonNull::new(raw).ok_or(AllocError)
}

/// Free a block obtained from [`try_alloc`] with the same `layout`.
///
/// # Safety
/// `ptr` must have come from `try_alloc(layout)` and not been freed before.
pub unsafe fn try_free(ptr: NonNull<u8>, layout: Layout) {
    if layout.size() == 0 {
        return;
    }
    // SAFETY: ptr/layout pair originate from try_alloc per the contract above.
    unsafe { GLOBAL.dealloc(ptr.as_ptr(), layout) }
}

/// A heap box over the nonpaged pool with a fallible constructor. Think `Box<T>`, but
/// `try_new` returns `Err` instead of aborting when the pool is exhausted.
pub struct KBox<T> {
    ptr: NonNull<T>,
}

impl<T> KBox<T> {
    /// Allocate and move `value` onto the pool. `Err(AllocError)` on pool exhaustion.
    pub fn try_new(value: T) -> Result<KBox<T>, AllocError> {
        let layout = Layout::new::<T>();
        if layout.size() == 0 {
            // ZST: no allocation; a dangling, aligned pointer is valid to form &T.
            return Ok(KBox { ptr: NonNull::dangling() });
        }
        let raw = try_alloc(layout)?.cast::<T>();
        // SAFETY: raw is freshly allocated, sized and aligned for T, uninitialized.
        unsafe { raw.as_ptr().write(value) };
        Ok(KBox { ptr: raw })
    }
}

impl<T> Deref for KBox<T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: ptr is valid and initialized for the lifetime of self.
        unsafe { self.ptr.as_ref() }
    }
}

impl<T> DerefMut for KBox<T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: unique borrow of self grants unique access to the owned T.
        unsafe { self.ptr.as_mut() }
    }
}

impl<T> Drop for KBox<T> {
    fn drop(&mut self) {
        let layout = Layout::new::<T>();
        // SAFETY: drop the owned value in place, then release its pool block.
        unsafe {
            core::ptr::drop_in_place(self.ptr.as_ptr());
            if layout.size() != 0 {
                try_free(self.ptr.cast::<u8>(), layout);
            }
        }
    }
}

// KBox owns its T, so it is Send/Sync exactly when T is.
unsafe impl<T: Send> Send for KBox<T> {}
unsafe impl<T: Sync> Sync for KBox<T> {}

/// Exhaust the pool on purpose and prove failure is graceful.
///
/// Allocates `block_size`-byte blocks, threading each on an intrusive
/// [`SINGLE_LIST_ENTRY`] chain (no side bookkeeping, O(1) stack), until `try_alloc`
/// returns `Err` — the pool said no, and we observed it as a value, not a crash. Every
/// block is then freed. Returns:
///   * `Ok(count)` — a graceful OOM was observed after `count` successful blocks, or the
///     safety cap was reached first (still no crash);
///   * it never returns `Err`, because allocation failure is not an error here, it is the
///     thing being demonstrated.
///
/// `block_size` is clamped to a sane minimum so each block can hold the chain pointer.
pub fn oom_probe(block_size: usize, safety_cap: usize) -> usize {
    let size = if block_size < core::mem::size_of::<SINGLE_LIST_ENTRY>() {
        core::mem::size_of::<SINGLE_LIST_ENTRY>()
    } else {
        block_size
    };
    let layout = match Layout::from_size_align(size, 16) {
        Ok(l) => l,
        Err(_) => return 0,
    };

    let mut head: *mut SINGLE_LIST_ENTRY = core::ptr::null_mut();
    let mut count = 0usize;

    while count < safety_cap {
        match try_alloc(layout) {
            Ok(p) => {
                let node = p.cast::<SINGLE_LIST_ENTRY>().as_ptr();
                // SAFETY: node points at a fresh block >= size_of::<SINGLE_LIST_ENTRY>.
                unsafe { (*node).Next = head };
                head = node;
                count += 1;
            }
            Err(AllocError) => break, // graceful OOM: the whole point of the probe.
        }
    }

    // Unwind the chain, freeing every block we took.
    while !head.is_null() {
        // SAFETY: head is one of our blocks; read its Next before freeing it.
        let next = unsafe { (*head).Next };
        // SAFETY: block came from try_alloc(layout); free with the same layout.
        unsafe { try_free(NonNull::new_unchecked(head as *mut u8), layout) };
        head = next;
    }

    count
}
