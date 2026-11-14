//! Host differential wrapper: compiles the kernel validator source unchanged.
//!
//! This crate exists only so the C++ differential harness can link the IO-3
//! name-grafting validator on the host. It includes the *same* `src/reparse_graft.rs`
//! used by the kernel staticlib (via `#[path]`), so there is one source of truth. The
//! validator is panic-free by construction; the panic handler below is a required
//! no_std stub that is never reached.
#![no_std]

use core::panic::PanicInfo;

#[path = "../../../src/reparse_graft.rs"]
pub mod reparse_graft;

#[panic_handler]
fn on_panic(_info: &PanicInfo) -> ! {
    // Unreachable for this validator (all paths are checked). Abort if ever hit.
    loop {
        core::hint::spin_loop();
    }
}

/// Host-only alias so Stanley's `t_reparse.c` links our Rust as the production symbol
/// `IopValidateMountPointBuffer` (NTAPI is empty on host; ULONG == u32 in his ntoskrnl.h).
/// This lets the real 3-way run (rust vs reparse.c vs Oracle) use the shipped harness.
#[no_mangle]
pub unsafe extern "C" fn IopValidateMountPointBuffer(
    buffer: *const core::ffi::c_void,
    buffer_length: u32,
) -> i32 {
    reparse_graft::leeway_rs_poc_validate_mount_point(buffer, buffer_length)
}
