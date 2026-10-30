# LiberNT Update, stage 1 core (libservicing). STANDALONE: no parent CMakeLists.txt includes this
# directory, so it is not part of the global build or the ISO (owner condition 3).
#
# To try it in a local, uncommitted tree add to base/system/CMakeLists.txt:
#     add_subdirectory(servicing/libservicing)
# and to base/applications/cmdutils/CMakeLists.txt:
#     add_subdirectory(libernt-update)
# The host build used for the stage 1 tests is tools/libernt-update/Build-And-Test.ps1.

list(APPEND SOURCE
    sha256.c
    util.c
    json.c
    target.c
    manifest.c
    verify.c
    journal.c
    store.c
    pending.c
    servicing.c)

add_library(libservicing ${SOURCE})
target_include_directories(libservicing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(libservicing psdk)

# unit tests (need signed fixtures: see tools/libernt-update/Build-And-Test.ps1; run on the VM or a host)
add_executable(libservicing_tests tests/tests.c)
set_module_type(libservicing_tests win32cui UNICODE)
target_link_libraries(libservicing_tests libservicing)
add_importlibs(libservicing_tests wintrust crypt32 msvcrt kernel32)
