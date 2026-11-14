//! LRM-RS — special-pool orchestration layer (on top of the engine in `lib.rs`).
//!
//! Clean-room Rust of the NT special-pool glue contract (Dwight's `nvs/nt/ntspecial.c`, observed
//! via Ryan's `OBSERVED-CONTRACTS-SPECIAL-POOL-VERIFIER.md`): the `MmUseSpecialPool` routing
//! predicate and the free-time corruption classifier that maps to the exact
//! `SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION` (0xC1) sub-codes and bug-check parameters. Our code.
//!
//! The page machinery (acquire a guarded page, free to a delayed-reuse window, classify an
//! address) is Stanley's `MiGuard*` (`nvs/sys/sysguard.c`); it is abstracted here as the
//! `GuardRegion` FFI trait so the orchestration can run over the real C provider in-kernel or a
//! mock in host tests. Bug checks are returned as a `BugCheck` value (never a direct trap), so
//! the kernel shim raises `KeBugCheckEx` on `Err` and the host harness asserts the code+params.
//!
//! NOTE (dependency): the full `MmAllocateSpecialPool`/`MmFreeSpecialPool` ALSO decode paged vs
//! non-paged and cache alignment from `POOL_TYPE` bits (`EX_POOL_BASE_TYPE_MASK`,
//! `CACHE_ALIGNED_POOL_MASK` in `expool.h`) — not reproduced here because those exact mask values
//! are not in the observed-contract doc and must not be guessed; they are passed in as decoded
//! booleans. Wiring the decode is a follow-up once `expool.h` is available.

use crate::rust_pool_special_select;

/// `SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION` and its free-time sub-codes (public ABI; a debugger
/// and the tests depend on these exact numbers / parameter positions).
pub const BUGCHECK_SPECIAL_POOL: u32 = 0xC1;
const SUB_NOT_ALLOCATED: u32 = 0x20; // double free / not special pool
const SUB_CORRUPT_BEFORE: u32 = 0x23; // byte before the buffer overwritten
const SUB_CORRUPT_AFTER: u32 = 0x24; // byte after the buffer overwritten
const SUB_FREE_IRQL: u32 = 0x31; // IRQL too high at free for the block's pool type

// Standard NT IRQLs (architecture constants, not expool-specific).
const APC_LEVEL: u8 = 1;
const DISPATCH_LEVEL: u8 = 2;

// Engine POOL_SPECIAL_STATUS (mirror of lib.rs).
const POOL_INTACT: i32 = 0;
const POOL_CORRUPT_BEFORE: i32 = 1;
const POOL_CORRUPT_AFTER: i32 = 2;

/// Guard-page region state for an address (mirror of `MiGuardState`).
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum GuardState {
    Outside = 0,
    Live = 1,
    Freed = 2,
}

/// The MiGuard* page provider, abstracted. `query` returns the state and, for a Live/Freed
/// address, the backing page base and the per-block record context.
pub trait GuardRegion {
    fn query(&self, addr: usize) -> (GuardState, usize, *mut core::ffi::c_void);
    fn allocate(&mut self) -> Option<(usize, *mut core::ffi::c_void)>;
    fn free(&mut self, page: usize) -> bool;
    fn refill(&mut self);
}

/// A pending controlled bug check: the code and its four NT parameters. The caller (kernel shim)
/// turns this into `KeBugCheckEx(code, p1, p2, p3, p4)`; a host test asserts the fields.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct BugCheck {
    pub code: u32,
    pub p1: usize,
    pub p2: usize,
    pub p3: usize,
    pub p4: usize,
}

/// `MmUseSpecialPool` routing predicate: special pool must be initialised (`region_present`) and
/// the allocation selected by the configured `special_pool_tag` (via the engine's selector).
pub fn use_special_pool(region_present: bool, special_pool_tag: u32, bytes: usize, tag: u32) -> bool {
    region_present && rust_pool_special_select(special_pool_tag, bytes, tag) != 0
}

/// The free-time decision for `MmFreeSpecialPool`, as a pure function of the already-gathered
/// inputs (so it is testable without the kernel): the guard-query result, whether the block owns
/// the pointer, the IRQL + whether the block is paged, and the engine's fill-pattern check.
/// Returns `Ok(())` to proceed with the actual free, or the exact `BugCheck` to raise. The order
/// matches `MmFreeSpecialPool`: not-allocated, then IRQL, then fill corruption.
#[allow(clippy::too_many_arguments)]
pub fn free_classify(
    p: usize,
    query: GuardState,
    owns: bool,
    irql: u8,
    paged: bool,
    flags: usize,
    check_status: i32,
    corrupt: usize,
) -> Result<(), BugCheck> {
    // Not a live, owned special-pool block (double free / wrong address): 0x20, param1 = P.
    if query != GuardState::Live || !owns {
        return Err(BugCheck { code: BUGCHECK_SPECIAL_POOL, p1: p, p2: 0, p3: 0, p4: SUB_NOT_ALLOCATED as usize });
    }
    // IRQL too high for the block's pool type: 0x31, params (Irql, Flags, P).
    if irql > DISPATCH_LEVEL || (paged && irql > APC_LEVEL) {
        return Err(BugCheck { code: BUGCHECK_SPECIAL_POOL, p1: irql as usize, p2: flags, p3: p, p4: SUB_FREE_IRQL as usize });
    }
    // Fill-pattern corruption: 0x23 before / 0x24 after, params (P, corrupt-address).
    match check_status {
        POOL_CORRUPT_BEFORE => Err(BugCheck { code: BUGCHECK_SPECIAL_POOL, p1: p, p2: corrupt, p3: 0, p4: SUB_CORRUPT_BEFORE as usize }),
        POOL_CORRUPT_AFTER => Err(BugCheck { code: BUGCHECK_SPECIAL_POOL, p1: p, p2: corrupt, p3: 0, p4: SUB_CORRUPT_AFTER as usize }),
        POOL_INTACT => Ok(()),
        _ => Ok(()),
    }
}

// ---- C ABI for the host differential / a future kernel shim ----

/// `MmUseSpecialPool(bytes, tag)` with the region/selector state passed explicitly.
#[no_mangle]
pub extern "C" fn lrm_mm_use_special_pool(region_present: u8, special_pool_tag: u32, bytes: usize, tag: u32) -> u8 {
    use_special_pool(region_present != 0, special_pool_tag, bytes, tag) as u8
}

/// Free-time classifier over the C ABI: writes the `BugCheck` to `*out` and returns 1 if a bug
/// check must be raised, 0 if the free may proceed.
///
/// # Safety
/// `out` must be a writable `BugCheck`.
#[no_mangle]
pub unsafe extern "C" fn lrm_mm_free_classify(
    p: usize,
    query: i32,
    owns: u8,
    irql: u8,
    paged: u8,
    flags: usize,
    check_status: i32,
    corrupt: usize,
    out: *mut BugCheck,
) -> u8 {
    let qs = match query {
        1 => GuardState::Live,
        2 => GuardState::Freed,
        _ => GuardState::Outside,
    };
    match free_classify(p, qs, owns != 0, irql, paged != 0, flags, check_status, corrupt) {
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
