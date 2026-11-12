# PROJECT:     LiberNT host-native tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     FsRtl file system filter callback dispatch host-test build
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Standalone, like submodules/host-tests/cmlib: compiles the real
# ntoskrnl/fsrtl/fsfltcb.c, unchanged, against the kernel shim in shim/ (found
# ahead of any real header) and runs each case in its own process.
# Configure with -S submodules/host-tests/fsfltcb.

cmake_minimum_required(VERSION 3.16)
project(fsfltcb_host_tests LANGUAGES C)
include(CTest)

get_filename_component(REACTOS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_executable(fsfltcb
    "${REACTOS_SOURCE_ROOT}/ntoskrnl/fsrtl/fsfltcb.c"
    t_fsfltcb.c)
target_include_directories(fsfltcb PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shim")
if(NOT MSVC)
    target_compile_options(fsfltcb PRIVATE -Wall -Wno-multichar -g)
endif()

option(FSFLTCB_SANITIZE "Build the FsRtl filter callback host tests under ASan/UBSan" OFF)
if(FSFLTCB_SANITIZE AND NOT MSVC)
    target_compile_options(fsfltcb PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fsfltcb PRIVATE -fsanitize=address,undefined)
endif()

set(FSFLTCB_CASES
    section_sync_calls_every_filter_in_order
    contexts_start_null
    failing_pre_unwinds
    filter_completes_operation
    informational_pre_status_still_locks
    cc_flush_pair_is_dispatched
    cc_flush_failing_pre_fails_the_acquire
    mod_write_pair_is_dispatched
    failing_release_pre_still_releases
    short_registration_is_not_read_past
    registration_rejects_a_size_without_callbacks
    no_filters_plain_locking)
foreach(case IN LISTS FSFLTCB_CASES)
    add_test(NAME fsfltcb_${case} COMMAND fsfltcb ${case})
    set_tests_properties(fsfltcb_${case} PROPERTIES TIMEOUT 20)
endforeach()
