//! NDK struct bindings with generated size/offset layout assertions (acceptance test 4).
//!
//! PROVENANCE: these definitions follow `bindgen`'s output for the ReactOS/LiberNT NDK
//! headers (`ntdef.h`, `ntbasedef.h`) on the x86_64 target. `bindgen` is not vendored in
//! the reproducible CI image (no libclang), so the checked-in copy is authoritative and
//! hand-verified against the documented NT x64 ABI; regenerate with `tools/gen-ndk.sh`
//! when libclang + the NDK headers are available and diff against this file.
//!
//! The point of this module is layout correctness: every bound type carries compile-time
//! `size_of` / `offset_of!` assertions, so a mismatch between Rust's view and the NT ABI
//! fails the build instead of corrupting kernel memory at runtime. `SINGLE_LIST_ENTRY`
//! is additionally used for real by the allocator OOM probe (see `alloc_pool`).
#![allow(non_camel_case_types, non_snake_case)]

use core::mem::{align_of, offset_of, size_of};

/// Compile-time assertion: a `false` condition is a build error, not a runtime fault.
macro_rules! const_assert {
    ($cond:expr, $msg:expr) => {
        const _: () = assert!($cond, $msg);
    };
}

// ---- Scalar NT typedefs (x64) ------------------------------------------------------
/// `KSPIN_LOCK` is `ULONG_PTR` (pointer-width). A zeroed value is the initialized,
/// unowned state, which is what `KeInitializeSpinLock` writes.
pub type KSPIN_LOCK = usize;
/// `KIRQL` is a single byte on every NT architecture.
pub type KIRQL = u8;

const_assert!(size_of::<KSPIN_LOCK>() == 8, "KSPIN_LOCK must be 8 bytes on x64");
const_assert!(size_of::<KIRQL>() == 1, "KIRQL must be 1 byte");

// ---- SINGLE_LIST_ENTRY -------------------------------------------------------------
/// `typedef struct _SINGLE_LIST_ENTRY { struct _SINGLE_LIST_ENTRY *Next; }`
#[repr(C)]
pub struct SINGLE_LIST_ENTRY {
    pub Next: *mut SINGLE_LIST_ENTRY,
}

const_assert!(size_of::<SINGLE_LIST_ENTRY>() == 8, "SINGLE_LIST_ENTRY size");
const_assert!(align_of::<SINGLE_LIST_ENTRY>() == 8, "SINGLE_LIST_ENTRY align");
const_assert!(offset_of!(SINGLE_LIST_ENTRY, Next) == 0, "SINGLE_LIST_ENTRY.Next offset");

// ---- LIST_ENTRY --------------------------------------------------------------------
/// `typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; }`
#[repr(C)]
pub struct LIST_ENTRY {
    pub Flink: *mut LIST_ENTRY,
    pub Blink: *mut LIST_ENTRY,
}

const_assert!(size_of::<LIST_ENTRY>() == 16, "LIST_ENTRY size");
const_assert!(align_of::<LIST_ENTRY>() == 8, "LIST_ENTRY align");
const_assert!(offset_of!(LIST_ENTRY, Flink) == 0, "LIST_ENTRY.Flink offset");
const_assert!(offset_of!(LIST_ENTRY, Blink) == 8, "LIST_ENTRY.Blink offset");

// ---- UNICODE_STRING ----------------------------------------------------------------
/// `typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; }`.
/// The classic ABI gotcha: 4 bytes of padding sit between `MaximumLength` and the
/// 8-byte-aligned `Buffer`, so the struct is 16 bytes with `Buffer` at offset 8.
#[repr(C)]
pub struct UNICODE_STRING {
    pub Length: u16,
    pub MaximumLength: u16,
    pub Buffer: *mut u16,
}

const_assert!(size_of::<UNICODE_STRING>() == 16, "UNICODE_STRING size (incl. padding)");
const_assert!(align_of::<UNICODE_STRING>() == 8, "UNICODE_STRING align");
const_assert!(offset_of!(UNICODE_STRING, Length) == 0, "UNICODE_STRING.Length offset");
const_assert!(offset_of!(UNICODE_STRING, MaximumLength) == 2, "UNICODE_STRING.MaximumLength offset");
const_assert!(offset_of!(UNICODE_STRING, Buffer) == 8, "UNICODE_STRING.Buffer offset (after 4B pad)");
