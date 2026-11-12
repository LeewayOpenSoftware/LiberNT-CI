# lnrspoc - experimental Rust kernel-driver PoC (v1).
#
# Built ONLY when LIBERNT_ENABLE_RUST=ON (guarded by the parent tools/CMakeLists.txt,
# default OFF). The driver is deliberately NEVER add_cd_file'd, so it is never placed
# on installation media. amd64 only (Leeway x64). ntoskrnl is untouched.
#
# Integration here is a self-contained custom cargo invocation (no vendored corrosion
# module). Corrosion (corrosion-rs/corrosion, MIT) is the recommended productization
# path once the approach is accepted.

if(NOT ARCH STREQUAL "amd64")
    message(STATUS "lnrspoc: Rust PoC is amd64-only; skipped for ARCH=${ARCH}")
    return()
endif()

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR "lnrspoc: LIBERNT_ENABLE_RUST=ON but 'cargo' was not found on PATH")
endif()

option(LNRSPOC_TEST_IOCTLS
    "Build lnrspoc with the destructive panic/bugcheck test IOCTL (disposable VM only)" OFF)

set(RUST_POC_TARGET x86_64-unknown-libernt-kernel)
set(RUST_POC_OUT ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(RUST_POC_LIB ${RUST_POC_OUT}/${RUST_POC_TARGET}/release/libleeway_rs_poc.a)
set(RUST_POC_FEATURES "")
if(LNRSPOC_TEST_IOCTLS)
    set(RUST_POC_FEATURES --features test-ioctls)
endif()

file(GLOB_RECURSE RUST_POC_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/${RUST_POC_TARGET}.json)

# The pinned toolchain (rust-toolchain.toml) and build-std/custom-target config
# (.cargo/config.toml) are picked up automatically from the crate directory.
add_custom_command(
    OUTPUT ${RUST_POC_LIB}
    COMMAND ${CMAKE_COMMAND} -E env
            CARGO_TARGET_DIR=${RUST_POC_OUT}
            ${CARGO_EXECUTABLE} build --release
            --locked
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
            ${RUST_POC_FEATURES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${RUST_POC_SRC}
    COMMENT "lnrspoc: building Rust staticlib (leeway_rs_poc) with the pinned toolchain"
    VERBATIM)
add_custom_target(lnrspoc_rust DEPENDS ${RUST_POC_LIB})

add_library(leeway_rs_poc STATIC IMPORTED GLOBAL)
set_target_properties(leeway_rs_poc PROPERTIES IMPORTED_LOCATION ${RUST_POC_LIB})
add_dependencies(leeway_rs_poc lnrspoc_rust)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(lnrspoc MODULE driver/lnrspoc.c)
set_module_type(lnrspoc kernelmodedriver)
add_importlibs(lnrspoc ntoskrnl hal)
target_link_libraries(lnrspoc leeway_rs_poc)
add_dependencies(lnrspoc lnrspoc_rust)
if(LNRSPOC_TEST_IOCTLS)
    target_compile_definitions(lnrspoc PRIVATE LNRSPOC_TEST_IOCTLS)
endif()

# Intentionally NO add_cd_file(): lnrspoc.sys is an experiment and must never be
# placed on installation media.
