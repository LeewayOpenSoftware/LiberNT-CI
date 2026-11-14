# PROJECT:     LiberNT build system
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Pinned Rust toolchain and reproducible no_std staticlib builds
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Rust code is built with one pinned STABLE toolchain and the built-in
# x86_64-pc-windows-gnu target, using the precompiled core/alloc of that
# toolchain (no -Zbuild-std, no custom target JSON, no nightly). Builds are
# --locked --offline: a crate that needs a registry or git dependency fails
# instead of reaching the network. The recipe lives in rust-build.cmake so that
# the build and the reproducibility CI job run exactly the same steps.
#
# Everything here is inert unless LIBERNT_ENABLE_RUST is ON (default OFF).

option(LIBERNT_ENABLE_RUST "Build Rust components (experimental, not part of the ISO)" OFF)

set(LIBERNT_RUST_TOOLCHAIN "1.97.1" CACHE STRING "Pinned Rust toolchain (rustup name)")
set(LIBERNT_RUST_TARGET "x86_64-pc-windows-gnu" CACHE STRING "Rust target for kernel-mode staticlibs")

set(_LIBERNT_RUST_BUILD_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/rust-build.cmake)

function(_libernt_rust_check_toolchain)
    if(DEFINED CACHE{LIBERNT_RUSTUP_EXECUTABLE} AND LIBERNT_RUST_CHECKED STREQUAL "${LIBERNT_RUST_TOOLCHAIN}/${LIBERNT_RUST_TARGET}")
        return()
    endif()

    find_program(LIBERNT_RUSTUP_EXECUTABLE rustup HINTS $ENV{CARGO_HOME}/bin $ENV{USERPROFILE}/.cargo/bin $ENV{HOME}/.cargo/bin NO_CMAKE_FIND_ROOT_PATH)
    if(NOT LIBERNT_RUSTUP_EXECUTABLE)
        message(FATAL_ERROR "LIBERNT_ENABLE_RUST=ON needs rustup. Install it, then run:\n"
                            "  rustup toolchain install ${LIBERNT_RUST_TOOLCHAIN} --profile minimal --component llvm-tools --target ${LIBERNT_RUST_TARGET}")
    endif()

    execute_process(COMMAND ${LIBERNT_RUSTUP_EXECUTABLE} run ${LIBERNT_RUST_TOOLCHAIN} rustc -vV
                    OUTPUT_VARIABLE _vv RESULT_VARIABLE _rc ERROR_QUIET)
    string(REGEX MATCH "release: ([^\n]+)" _m "${_vv}")
    if(NOT _rc EQUAL 0 OR NOT CMAKE_MATCH_1 STREQUAL LIBERNT_RUST_TOOLCHAIN)
        message(FATAL_ERROR "Rust toolchain ${LIBERNT_RUST_TOOLCHAIN} is not installed (found '${CMAKE_MATCH_1}'). Run:\n"
                            "  rustup toolchain install ${LIBERNT_RUST_TOOLCHAIN} --profile minimal --component llvm-tools --target ${LIBERNT_RUST_TARGET}")
    endif()

    execute_process(COMMAND ${LIBERNT_RUSTUP_EXECUTABLE} run ${LIBERNT_RUST_TOOLCHAIN} rustc --print target-libdir --target ${LIBERNT_RUST_TARGET}
                    OUTPUT_VARIABLE _libdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(GLOB _core "${_libdir}/libcore-*.rlib")
    if(NOT _core)
        message(FATAL_ERROR "Rust target ${LIBERNT_RUST_TARGET} is missing from ${LIBERNT_RUST_TOOLCHAIN}. Run:\n"
                            "  rustup target add --toolchain ${LIBERNT_RUST_TOOLCHAIN} ${LIBERNT_RUST_TARGET}")
    endif()

    execute_process(COMMAND ${LIBERNT_RUSTUP_EXECUTABLE} run ${LIBERNT_RUST_TOOLCHAIN} rustc --print sysroot
                    OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX MATCH "host: ([^\n]+)" _h "${_vv}")
    set(_bin "${_sysroot}/lib/rustlib/${CMAKE_MATCH_1}/bin")
    find_program(LIBERNT_RUST_LLVM_AR llvm-ar PATHS ${_bin} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_program(LIBERNT_RUST_LLVM_NM llvm-nm PATHS ${_bin} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_program(LIBERNT_RUST_LLVM_OBJDUMP llvm-objdump PATHS ${_bin} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT LIBERNT_RUST_LLVM_AR OR NOT LIBERNT_RUST_LLVM_NM OR NOT LIBERNT_RUST_LLVM_OBJDUMP)
        message(FATAL_ERROR "The llvm-tools component of Rust ${LIBERNT_RUST_TOOLCHAIN} is missing. Run:\n"
                            "  rustup component add --toolchain ${LIBERNT_RUST_TOOLCHAIN} llvm-tools")
    endif()

    message(STATUS "Rust: toolchain ${LIBERNT_RUST_TOOLCHAIN}, target ${LIBERNT_RUST_TARGET}")
    set(LIBERNT_RUST_CHECKED "${LIBERNT_RUST_TOOLCHAIN}/${LIBERNT_RUST_TARGET}" CACHE INTERNAL "")
endfunction()

# add_rust_staticlib(<target>
#     MANIFEST_DIR <crate directory>
#     CRATE <library name, as in [lib] name>
#     [FEATURES <feature>...]
#     [ALLOWED_EXTERNS <symbol>...])
#
# Builds the crate as a staticlib and exposes ONLY the crate's own objects as
# the imported static library <target>: the compiler_builtins members that
# cargo bundles are dropped, so the kernel link resolves runtime helpers from
# its own libraries. The build fails if the crate's objects reference any
# symbol outside ALLOWED_EXTERNS, or contain x87, MMX or AVX instructions.
function(add_rust_staticlib _target)
    cmake_parse_arguments(_R "" "MANIFEST_DIR;CRATE" "FEATURES;ALLOWED_EXTERNS" ${ARGN})
    if(NOT _R_MANIFEST_DIR OR NOT _R_CRATE)
        message(FATAL_ERROR "add_rust_staticlib(${_target}): MANIFEST_DIR and CRATE are required")
    endif()

    _libernt_rust_check_toolchain()

    set(_work ${CMAKE_CURRENT_BINARY_DIR}/${_target}.rust)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/lib${_target}.a)

    file(GLOB_RECURSE _src CONFIGURE_DEPENDS
         ${_R_MANIFEST_DIR}/src/*.rs
         ${_R_MANIFEST_DIR}/Cargo.toml
         ${_R_MANIFEST_DIR}/Cargo.lock)

    string(REPLACE ";" "," _features "${_R_FEATURES}")
    string(REPLACE ";" "," _externs "${_R_ALLOWED_EXTERNS}")

    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND}
                -DRUSTUP=${LIBERNT_RUSTUP_EXECUTABLE}
                -DTOOLCHAIN=${LIBERNT_RUST_TOOLCHAIN}
                -DRUST_TARGET=${LIBERNT_RUST_TARGET}
                -DLLVM_AR=${LIBERNT_RUST_LLVM_AR}
                -DLLVM_NM=${LIBERNT_RUST_LLVM_NM}
                -DLLVM_OBJDUMP=${LIBERNT_RUST_LLVM_OBJDUMP}
                -DMANIFEST_DIR=${_R_MANIFEST_DIR}
                -DCRATE=${_R_CRATE}
                -DWORK_DIR=${_work}
                -DOUTPUT=${_out}
                -DFEATURES=${_features}
                -DALLOWED_EXTERNS=${_externs}
                -P ${_LIBERNT_RUST_BUILD_SCRIPT}
        DEPENDS ${_src} ${_LIBERNT_RUST_BUILD_SCRIPT}
        COMMENT "Rust: building ${_R_CRATE} with ${LIBERNT_RUST_TOOLCHAIN}"
        VERBATIM)

    add_custom_target(${_target}_rust DEPENDS ${_out})
    add_library(${_target} STATIC IMPORTED GLOBAL)
    set_target_properties(${_target} PROPERTIES IMPORTED_LOCATION ${_out})
    add_dependencies(${_target} ${_target}_rust)
endfunction()
