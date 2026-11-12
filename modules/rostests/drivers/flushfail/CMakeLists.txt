
# flushfail: a test-only volume filter (rejects cache flushes, reports a
# volatile write cache without FUA) and the tool that loads and drives it.
# Test suite only: never on the install media, no .inf.

add_library(flushfail_drv MODULE flushfail_drv.c)
set_module_type(flushfail_drv kernelmodedriver)
add_importlibs(flushfail_drv ntoskrnl hal)
add_rostests_file(TARGET flushfail_drv SUBDIR flushfail)

add_executable(flushfail flushfail.c)
set_module_type(flushfail win32cui UNICODE)
add_importlibs(flushfail advapi32 msvcrt kernel32)
add_rostests_file(TARGET flushfail SUBDIR flushfail)
