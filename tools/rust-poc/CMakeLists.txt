# lnrspoc - experimental Rust kernel-driver PoC (v1).
#
# Built ONLY when LIBERNT_ENABLE_RUST=ON (guarded by the parent tools/CMakeLists.txt,
# default OFF). The driver is deliberately NEVER add_cd_file'd, so it is never placed
# on installation media. amd64 only (Leeway x64). ntoskrnl is untouched.
#
# The Rust staticlib is built by add_rust_staticlib() (sdk/cmake/rust.cmake).

if(NOT ARCH STREQUAL "amd64")
    message(STATUS "lnrspoc: Rust PoC is amd64-only; skipped for ARCH=${ARCH}")
    return()
endif()

option(LNRSPOC_TEST_IOCTLS
    "Build lnrspoc with the destructive panic/bugcheck test IOCTL (disposable VM only)" OFF)

set(RUST_POC_FEATURES "")
set(RUST_POC_EXTERNS ExAllocatePoolWithTag ExFreePoolWithTag KeInitializeSpinLock
                     KeAcquireSpinLockRaiseToDpc KeReleaseSpinLock)
if(LNRSPOC_TEST_IOCTLS)
    set(RUST_POC_FEATURES test-ioctls)
    list(APPEND RUST_POC_EXTERNS KeBugCheckEx)
endif()

# Pinned toolchain, reproducible flags and the extern/opcode gate: sdk/cmake/rust.cmake.
add_rust_staticlib(leeway_rs_poc
    MANIFEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}
    CRATE leeway_rs_poc
    FEATURES ${RUST_POC_FEATURES}
    ALLOWED_EXTERNS ${RUST_POC_EXTERNS})

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(lnrspoc MODULE driver/lnrspoc.c)
set_module_type(lnrspoc kernelmodedriver)
add_importlibs(lnrspoc ntoskrnl hal)
target_link_libraries(lnrspoc leeway_rs_poc)
if(LNRSPOC_TEST_IOCTLS)
    target_compile_definitions(lnrspoc PRIVATE LNRSPOC_TEST_IOCTLS)
endif()

# Intentionally NO add_cd_file(): lnrspoc.sys is an experiment and must never be
# placed on installation media.
