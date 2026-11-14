//! LRM-RS — special-pool orchestration layer (on top of the engine in `lib.rs`).
//!
//! Clean-room Rust of the NT special-pool glue contract (Dwight's `nvs/nt/ntspecial.c`, observed
//! via Ryan's `OBSERVED-CONTRACTS-SPECIAL-POOL-VERIFIER.md` and the recovered C reference): the
//! `MmUseSpecialPool` routing predicate, the full `MmAllocateSpecialPool`/`MmFreeSpecialPool`
//! orchestration over the guard-page provider, and the free-time corruption classifier that maps
//! to the exact `SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION` (0xC1) sub-codes and bug-check
//! parameters. Our code.
//!
//! The page machinery (acquire a guarded page, free to a delayed-reuse window, classify an
//! address) is Stanley's `MiGuard*` (`nvs/sys/sysguard.c`); it is abstracted here as the
//! `GuardRegion` FFI trait so the orchestration can run over the real C provider in-kernel or a
//! mock in host tests. Bug checks are returned as a `BugCheck` value (never a direct trap), so
//! the kernel shim raises `KeBugCheckEx` on `Err` and the host harness asserts the code+params.
//!
//! POOL_TYPE decode follows the recovered C reference exactly (`ntspecial.c` +
//! `expool.h::ExpPoolHeapFromType`): paged = `PoolType & EX_POOL_BASE_TYPE_MASK`; cache alignment
//! = `PoolType & CACHE_ALIGNED_POOL_MASK` → 64, else engine default; executable =
//! non-paged and NX bit clear. `CACHE_ALIGNED_POOL_MASK`/`POOL_NX_ALLOCATION` are the public NT
//! POOL_TYPE/WDM values (not in the private headers); the host differential feeds both the C and
//! Rust sides the identical value, so the equivalence proof is value-independent.

use crate::{
    rust_pool_special_check, rust_pool_special_owns, rust_pool_special_place,
    rust_pool_special_prepare, rust_pool_special_select, PoolSpecialRecord,
};
use core::ffi::c_void;

/// `SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION` and its sub-codes (public ABI; a debugger and the
/// tests depend on these exact numbers / parameter positions). Mirror of `ntspecial.c`.
pub const BUGCHECK_SPECIAL_POOL: u32 = 0xC1;
const SUB_NOT_ALLOCATED: u32 = 0x20; // double free / not special pool
const SUB_CORRUPT_BEFORE: u32 = 0x23; // byte before the buffer overwritten
const SUB_CORRUPT_AFTER: u32 = 0x24; // byte after the buffer overwritten
const SUB_ALLOCATE_IRQL: u32 = 0x30; // IRQL too high at allocate for the pool type
const SUB_FREE_IRQL: u32 = 0x31; // IRQL too high at free for the block's pool type

// Standard NT IRQLs (architecture constants, not expool-specific).
const APC_LEVEL: u8 = 1;
const DISPATCH_LEVEL: u8 = 2;

// POOL_TYPE decode (expool.h for the first three; public NT POOL_TYPE/WDM for the last two —
// see the module doc on why a host-stub value is sound for the differential).
const EX_POOL_BASE_TYPE_MASK: u32 = 1; // bit0 = paged
const CACHE_ALIGNED_POOL_MASK: u32 = 4; // POOL_TYPE cache-aligned variants (NonPagedPoolCacheAligned=4)
const POOL_NX_ALLOCATION: u32 = 0x200; // WDM: request non-executable non-paged pool
const MI_SPECIAL_POOL_CACHE_ALIGNMENT: u32 = 64;

// Engine POOL_SPECIAL_STATUS (mirror of lib.rs / poolspecial.h).
const POOL_INTACT: i32 = 0;
const POOL_CORRUPT_BEFORE: i32 = 1;
const POOL_CORRUPT_AFTER: i32 = 2;

/// Guard-page region state for an address — exact mirror of `MI_GUARD_KIND` (`misys.h`):
/// `MiGuardOutside=0, MiGuardGuardPage=1, MiGuardLive=2, MiGuardFreed=3`.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum GuardState {
    Outside = 0,
    GuardPage = 1,
    Live = 2,
    Freed = 3,
}

impl GuardState {
    #[inline]
    fn from_raw(k: i32) -> GuardState {
        match k {
            1 => GuardState::GuardPage,
            2 => GuardState::Live,
            3 => GuardState::Freed,
            _ => GuardState::Outside,
        }
    }
}

/// The MiGuard* page provider, abstracted. `query` returns the state and, for a Live/Freed
/// address, the backing page base and the per-block record context. `allocate` takes the decoded
/// `executable` bit (the kernel shim maps it to `MI_PROT_EXECUTE_READWRITE`/`MI_PROT_READWRITE`,
/// whose numeric values are not in the private headers — kept out of Rust on purpose).
pub trait GuardRegion {
    fn query(&self, addr: usize) -> (GuardState, usize, *mut c_void);
    fn allocate(&mut self, executable: bool) -> Option<(usize, *mut c_void)>;
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

impl BugCheck {
    #[inline]
    fn special(p1: usize, p2: usize, p3: usize, sub: u32) -> BugCheck {
        BugCheck { code: BUGCHECK_SPECIAL_POOL, p1, p2, p3, p4: sub as usize }
    }
}

/// `MmUseSpecialPool` routing predicate: special pool must be initialised (`region_present`) and
/// the allocation selected by the configured `special_pool_tag` (via the engine's selector).
pub fn use_special_pool(region_present: bool, special_pool_tag: u32, bytes: usize, tag: u32) -> bool {
    region_present && rust_pool_special_select(special_pool_tag, bytes, tag) != 0
}

/// `#[must_use]` outcome of `MmAllocateSpecialPool`: either a (possibly null) block pointer the
/// allocation produced / declined, or the exact `BugCheck` to raise.
pub type AllocResult = Result<*mut c_void, BugCheck>;

/// Full `MmAllocateSpecialPool(NumberOfBytes, Tag, PoolType, SpecialType)` over the `GuardRegion`.
/// Mirrors `ntspecial.c` exactly: null region → declined; IRQL too high → 0x30 bug check;
/// placement from `SpecialType`/underruns + cache alignment from `PoolType`; guard allocate /
/// placement failure → declined (null); else the prepared block pointer. `region_present` is
/// `MiSpecialPoolRegion != NULL`; `underruns` is `MiSpecialPoolUnderruns`.
#[allow(clippy::too_many_arguments)]
pub fn mm_allocate_special_pool<R: GuardRegion>(
    region: &mut R,
    region_present: bool,
    underruns: bool,
    bytes: usize,
    tag: u32,
    pool_type: u32,
    special_type: u32,
    irql: u8,
) -> AllocResult {
    if !region_present {
        return Ok(core::ptr::null_mut());
    }

    let paged = (pool_type & EX_POOL_BASE_TYPE_MASK) != 0;
    if irql > DISPATCH_LEVEL || (paged && irql > APC_LEVEL) {
        // KeBugCheckEx(0xC1, Irql, PoolType, NumberOfBytes, 0x30)
        return Err(BugCheck::special(irql as usize, pool_type as usize, bytes, SUB_ALLOCATE_IRQL));
    }

    // AtStart: SpecialType 0 = page end, 1 = page start, else = global default (underruns).
    let at_start = match special_type {
        0 => false,
        1 => true,
        _ => underruns,
    };
    // Cache alignment: 64 if the cache-aligned POOL_TYPE bit is set, else 0 (engine → ALIGNMENT).
    let alignment = if (pool_type & CACHE_ALIGNED_POOL_MASK) != 0 {
        MI_SPECIAL_POOL_CACHE_ALIGNMENT
    } else {
        0
    };

    let mut offset: u32 = 0;
    // SAFETY: `offset` is a valid writable u32.
    let placed = unsafe { rust_pool_special_place(bytes, alignment, at_start as u8, &mut offset) };
    if placed == 0 {
        return Ok(core::ptr::null_mut());
    }

    let executable = exp_pool_heap_executable(pool_type);
    let (page, record) = match region.allocate(executable) {
        Some(pr) => pr,
        None => return Ok(core::ptr::null_mut()),
    };

    // SAFETY: the provider guarantees `record` is a writable POOL_SPECIAL_RECORD and `page` a
    // writable PAGE_SIZE page for the engine (same contract as the kernel MiGuardAllocate).
    let block = unsafe {
        rust_pool_special_prepare(
            record as *mut PoolSpecialRecord,
            page as *mut c_void,
            bytes,
            offset,
            tag,
            pool_type,
        )
    };
    Ok(block)
}

/// `ExpPoolHeapFromType(PoolType) == EX_POOL_HEAP_EXECUTABLE`: non-paged AND NX bit clear.
#[inline]
fn exp_pool_heap_executable(pool_type: u32) -> bool {
    if (pool_type & EX_POOL_BASE_TYPE_MASK) != 0 {
        return false; // paged
    }
    (pool_type & POOL_NX_ALLOCATION) == 0
}

/// Full `MmFreeSpecialPool(P)` over the `GuardRegion`. Mirrors `ntspecial.c` order exactly:
/// not-live-or-not-owned → 0x20; IRQL too high for the block's pool type → 0x31; fill corruption
/// → 0x23/0x24 with the exact corrupt address; guard free failure → 0x20; then refill. `Ok(())`
/// means the free completed.
pub fn mm_free_special_pool<R: GuardRegion>(region: &mut R, p: usize, irql: u8) -> Result<(), BugCheck> {
    let (state, page, ctx) = region.query(p);

    // owns check via the engine (only meaningful with a record context).
    let owns = !ctx.is_null()
        // SAFETY: provider gives a readable POOL_SPECIAL_RECORD context for an in-region address.
        && unsafe {
            rust_pool_special_owns(ctx as *const PoolSpecialRecord, page as *mut c_void, p as *mut c_void)
        } != 0;

    if state != GuardState::Live || !owns {
        return Err(BugCheck::special(p, 0, 0, SUB_NOT_ALLOCATED));
    }

    let record = ctx as *const PoolSpecialRecord;
    // SAFETY: owns==true implies a valid record.
    let flags = unsafe { (*record).flags } as usize;
    let paged = (flags & EX_POOL_BASE_TYPE_MASK as usize) != 0;
    if irql > DISPATCH_LEVEL || (paged && irql > APC_LEVEL) {
        // KeBugCheckEx(0xC1, Irql, Record->Flags, P, 0x31)
        return Err(BugCheck::special(irql as usize, flags, p, SUB_FREE_IRQL));
    }

    let mut corrupt: usize = 0;
    // SAFETY: page readable for PAGE_SIZE, record readable, corrupt writable.
    let status = unsafe { rust_pool_special_check(record, page as *mut c_void, &mut corrupt) };
    match status {
        POOL_CORRUPT_BEFORE => return Err(BugCheck::special(p, corrupt, 0, SUB_CORRUPT_BEFORE)),
        POOL_CORRUPT_AFTER => return Err(BugCheck::special(p, corrupt, 0, SUB_CORRUPT_AFTER)),
        POOL_INTACT => {}
        _ => {}
    }

    if !region.free(page) {
        return Err(BugCheck::special(p, 0, 0, SUB_NOT_ALLOCATED));
    }
    region.refill();
    Ok(())
}

/// The free-time *classification* as a pure function of already-gathered inputs (retained for the
/// OBSERVED-CONTRACTS spec test and the kernel shim that gathers the inputs itself). Same order as
/// `mm_free_special_pool` minus the provider calls.
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
    if query != GuardState::Live || !owns {
        return Err(BugCheck::special(p, 0, 0, SUB_NOT_ALLOCATED));
    }
    if irql > DISPATCH_LEVEL || (paged && irql > APC_LEVEL) {
        return Err(BugCheck::special(irql as usize, flags, p, SUB_FREE_IRQL));
    }
    match check_status {
        POOL_CORRUPT_BEFORE => Err(BugCheck::special(p, corrupt, 0, SUB_CORRUPT_BEFORE)),
        POOL_CORRUPT_AFTER => Err(BugCheck::special(p, corrupt, 0, SUB_CORRUPT_AFTER)),
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
/// check must be raised, 0 if the free may proceed. `query` is a raw `MI_GUARD_KIND` value.
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
    match free_classify(p, GuardState::from_raw(query), owns != 0, irql, paged != 0, flags, check_status, corrupt) {
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

/// A C-side guard provider as a vtable of function pointers, so the host full-stack harness can
/// drive the Rust orchestration over the SAME mock provider it gives the C `ntspecial.c`.
#[repr(C)]
pub struct LrmGuardVtbl {
    /// query(this, addr, *out page, *out ctx) -> MI_GUARD_KIND
    pub query: extern "C" fn(*mut c_void, usize, *mut usize, *mut *mut c_void) -> i32,
    /// allocate(this, executable, *out page, *out ctx) -> 1 on success, 0 on failure
    pub allocate: extern "C" fn(*mut c_void, u8, *mut usize, *mut *mut c_void) -> u8,
    /// free(this, page) -> 1 on success, 0 on failure
    pub free: extern "C" fn(*mut c_void, usize) -> u8,
    /// refill(this)
    pub refill: extern "C" fn(*mut c_void),
    /// opaque provider state passed back as the first arg
    pub this: *mut c_void,
}

struct VtblRegion<'a>(&'a LrmGuardVtbl);

impl GuardRegion for VtblRegion<'_> {
    fn query(&self, addr: usize) -> (GuardState, usize, *mut c_void) {
        let mut page: usize = 0;
        let mut ctx: *mut c_void = core::ptr::null_mut();
        let k = (self.0.query)(self.0.this, addr, &mut page, &mut ctx);
        (GuardState::from_raw(k), page, ctx)
    }
    fn allocate(&mut self, executable: bool) -> Option<(usize, *mut c_void)> {
        let mut page: usize = 0;
        let mut ctx: *mut c_void = core::ptr::null_mut();
        if (self.0.allocate)(self.0.this, executable as u8, &mut page, &mut ctx) != 0 {
            Some((page, ctx))
        } else {
            None
        }
    }
    fn free(&mut self, page: usize) -> bool {
        (self.0.free)(self.0.this, page) != 0
    }
    fn refill(&mut self) {
        (self.0.refill)(self.0.this)
    }
}

/// `MmAllocateSpecialPool` over the C vtable provider. Returns 0 = declined (`*out_ptr` = block or
/// null), 1 = allocated (`*out_ptr` set), 2 = bug check (`*out_bc` set).
///
/// # Safety
/// `v`, `out_ptr`, `out_bc` must be valid; the vtable callbacks must honour the provider contract.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lrm_mm_allocate_special_pool(
    v: *const LrmGuardVtbl,
    region_present: u8,
    underruns: u8,
    bytes: usize,
    tag: u32,
    pool_type: u32,
    special_type: u32,
    irql: u8,
    out_ptr: *mut *mut c_void,
    out_bc: *mut BugCheck,
) -> u8 {
    // SAFETY: caller guarantees `v` valid.
    let mut region = VtblRegion(unsafe { &*v });
    match mm_allocate_special_pool(
        &mut region,
        region_present != 0,
        underruns != 0,
        bytes,
        tag,
        pool_type,
        special_type,
        irql,
    ) {
        Ok(ptr) => {
            if !out_ptr.is_null() {
                unsafe { *out_ptr = ptr };
            }
            if ptr.is_null() {
                0
            } else {
                1
            }
        }
        Err(bc) => {
            if !out_bc.is_null() {
                unsafe { *out_bc = bc };
            }
            2
        }
    }
}

/// `MmFreeSpecialPool` over the C vtable provider. Returns 0 = freed, 1 = bug check (`*out_bc`).
///
/// # Safety
/// `v`, `out_bc` must be valid; the vtable callbacks must honour the provider contract.
#[no_mangle]
pub unsafe extern "C" fn lrm_mm_free_special_pool(
    v: *const LrmGuardVtbl,
    p: usize,
    irql: u8,
    out_bc: *mut BugCheck,
) -> u8 {
    // SAFETY: caller guarantees `v` valid.
    let mut region = VtblRegion(unsafe { &*v });
    match mm_free_special_pool(&mut region, p, irql) {
        Ok(()) => 0,
        Err(bc) => {
            if !out_bc.is_null() {
                unsafe { *out_bc = bc };
            }
            1
        }
    }
}
