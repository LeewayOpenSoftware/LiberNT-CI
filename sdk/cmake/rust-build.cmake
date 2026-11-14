# PROJECT:     LiberNT build system
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Reproducible build of one no_std Rust staticlib (cmake -P script)
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Called by add_rust_staticlib() (rust.cmake) and by the reproducibility CI job.
# Inputs (-D): RUSTUP TOOLCHAIN RUST_TARGET LLVM_AR LLVM_NM LLVM_OBJDUMP
#              MANIFEST_DIR CRATE WORK_DIR OUTPUT [FEATURES] [ALLOWED_EXTERNS]
# FEATURES and ALLOWED_EXTERNS are comma separated.
#
# The result OUTPUT holds only the crate's own objects. It does not depend on
# the source or build directory, the user's cargo configuration or the
# network: cargo runs from WORK_DIR with a private CARGO_HOME, --locked and
# --offline, and every absolute path is remapped.

cmake_minimum_required(VERSION 3.17)

foreach(_v RUSTUP TOOLCHAIN RUST_TARGET LLVM_AR LLVM_NM LLVM_OBJDUMP MANIFEST_DIR CRATE WORK_DIR OUTPUT)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "rust-build: ${_v} is not set")
    endif()
endforeach()

get_filename_component(MANIFEST_DIR "${MANIFEST_DIR}" ABSOLUTE)
get_filename_component(WORK_DIR "${WORK_DIR}" ABSOLUTE)
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)

set(_target_dir "${WORK_DIR}/target")
set(_cargo_home "${WORK_DIR}/cargo-home")
set(_objs "${WORK_DIR}/objs")
file(MAKE_DIRECTORY "${_target_dir}" "${_cargo_home}")
file(REMOVE_RECURSE "${_objs}")
file(MAKE_DIRECTORY "${_objs}")

file(TO_NATIVE_PATH "${MANIFEST_DIR}" _src_native)
file(TO_NATIVE_PATH "${_target_dir}" _tgt_native)
set(_rustflags "-Csymbol-mangling-version=v0")
foreach(_pair "${MANIFEST_DIR}=/rust/${CRATE}" "${_src_native}=/rust/${CRATE}"
              "${_target_dir}=/rust-target" "${_tgt_native}=/rust-target")
    string(APPEND _rustflags " --remap-path-prefix=${_pair}")
endforeach()

set(_features_arg "")
if(FEATURES)
    set(_features_arg --features "${FEATURES}")
endif()

foreach(_tool cargo rustc)
    execute_process(COMMAND "${RUSTUP}" which --toolchain "${TOOLCHAIN}" ${_tool}
                    OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_path}")
        message(FATAL_ERROR "rust-build: ${_tool} of toolchain ${TOOLCHAIN} not found")
    endif()
    set(_${_tool} "${_path}")
endforeach()

set(ENV{RUSTUP_TOOLCHAIN} "${TOOLCHAIN}")
set(ENV{RUSTC} "${_rustc}")
set(ENV{CARGO_HOME} "${_cargo_home}")
set(ENV{CARGO_TARGET_DIR} "${_tgt_native}")
set(ENV{RUSTFLAGS} "${_rustflags}")
set(ENV{CARGO_INCREMENTAL} "0")
unset(ENV{CARGO_BUILD_RUSTFLAGS})
unset(ENV{CARGO_ENCODED_RUSTFLAGS})
unset(ENV{RUSTC_WRAPPER})

execute_process(
    COMMAND "${_cargo}" build --release --locked --offline
            --target "${RUST_TARGET}" --manifest-path "${MANIFEST_DIR}/Cargo.toml" ${_features_arg}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: cargo failed for ${CRATE} (${_rc})")
endif()

set(_lib "${_target_dir}/${RUST_TARGET}/release/lib${CRATE}.a")
if(NOT EXISTS "${_lib}")
    message(FATAL_ERROR "rust-build: ${_lib} was not produced")
endif()

execute_process(COMMAND "${LLVM_AR}" t "${_lib}" OUTPUT_VARIABLE _members RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: cannot list ${_lib}")
endif()
string(REPLACE "\n" ";" _members "${_members}")
set(_own "")
foreach(_m ${_members})
    if(_m MATCHES "^${CRATE}-")
        list(APPEND _own "${_m}")
    endif()
endforeach()
if(NOT _own)
    message(FATAL_ERROR "rust-build: no ${CRATE} objects in ${_lib}")
endif()
list(SORT _own)

execute_process(COMMAND "${LLVM_AR}" x "${_lib}" ${_own} WORKING_DIRECTORY "${_objs}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: cannot extract the ${CRATE} objects")
endif()

execute_process(COMMAND "${LLVM_NM}" --undefined-only --just-symbol-name ${_own}
                WORKING_DIRECTORY "${_objs}" OUTPUT_VARIABLE _undef RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: llvm-nm failed")
endif()
string(REPLACE "\n" ";" _undef "${_undef}")
string(REPLACE "," ";" _allowed "${ALLOWED_EXTERNS}")
set(_bad "")
foreach(_s ${_undef})
    if(NOT _s STREQUAL "" AND NOT _s IN_LIST _allowed)
        list(APPEND _bad "${_s}")
    endif()
endforeach()
if(_bad)
    list(REMOVE_DUPLICATES _bad)
    message(FATAL_ERROR "rust-build: ${CRATE} references symbols outside its allowed list: ${_bad}")
endif()

execute_process(COMMAND "${LLVM_OBJDUMP}" -d --no-show-raw-insn ${_own}
                WORKING_DIRECTORY "${_objs}" OUTPUT_VARIABLE _dis RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: llvm-objdump failed")
endif()
if(_dis MATCHES "\n *[0-9a-f]+:[ \t]+(f[a-z0-9]+|emms)[ \t\n]")
    message(FATAL_ERROR "rust-build: ${CRATE} contains x87/MMX code ('${CMAKE_MATCH_1}')")
endif()
if(_dis MATCHES "%[yz]mm|vzeroupper")
    message(FATAL_ERROR "rust-build: ${CRATE} contains AVX code")
endif()

file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${LLVM_AR}" rcsD "${OUTPUT}" ${_own} WORKING_DIRECTORY "${_objs}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rust-build: cannot write ${OUTPUT}")
endif()

file(SHA256 "${OUTPUT}" _hash)
get_filename_component(_name "${OUTPUT}" NAME)
file(WRITE "${OUTPUT}.sha256" "${_hash}  ${_name}\n")
message(STATUS "rust-build: ${CRATE} ${TOOLCHAIN} ${RUST_TARGET} sha256 ${_hash}")
