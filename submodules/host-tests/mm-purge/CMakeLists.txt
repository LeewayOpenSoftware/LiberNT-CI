# PROJECT:  LiberNT host-native tests
# PURPOSE:  MiSegmentPurge outward-rounding regression (defect A-2). Self-contained
#           arithmetic guard (no MM link), like submodules/host-tests/cmlib's t_dirty_arith.
# Configure with -S submodules/host-tests/mm-purge
cmake_minimum_required(VERSION 3.16)
project(mm_purge_host_tests LANGUAGES C)
include(CTest)
add_executable(mm_purge_rounding t_purge_rounding.c)
if(NOT MSVC)
    target_compile_options(mm_purge_rounding PRIVATE -Wall -Wextra -O2)
endif()
add_test(NAME mm_purge_rounding COMMAND mm_purge_rounding)
set_tests_properties(mm_purge_rounding PROPERTIES TIMEOUT 20)
