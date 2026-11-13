# PROJECT:     LiberNT host-native tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     I/O manager host tests: the mount-point reparse buffer validator
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Standalone, like submodules/host-tests/cmlib: compiles the real
# ntoskrnl/io/iomgr/reparse.c against the ntoskrnl.h stand-in in this
# directory. Configure with -S submodules/host-tests/iomgr.

cmake_minimum_required(VERSION 3.16)
project(iomgr_host_tests LANGUAGES C)
include(CTest)

get_filename_component(REACTOS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_executable(iomgr_reparse t_reparse.c "${REACTOS_SOURCE_ROOT}/ntoskrnl/io/iomgr/reparse.c")
target_include_directories(iomgr_reparse PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
set_property(TARGET iomgr_reparse PROPERTY C_STANDARD 11)

option(IOMGR_SANITIZE "Build the I/O manager host tests under ASan/UBSan" OFF)
if(NOT MSVC)
    target_compile_options(iomgr_reparse PRIVATE -Wall -Wextra -g)
    if(IOMGR_SANITIZE)
        target_compile_options(iomgr_reparse PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(iomgr_reparse PRIVATE -fsanitize=address,undefined)
    endif()
endif()

add_test(NAME iomgr_reparse COMMAND iomgr_reparse 200000)
set_tests_properties(iomgr_reparse PROPERTIES TIMEOUT 60)
