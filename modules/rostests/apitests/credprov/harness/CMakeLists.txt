
add_executable(credprov_harness harness.c)
set_module_type(credprov_harness win32cui UNICODE)
target_link_libraries(credprov_harness credser uuid)
add_importlibs(credprov_harness secur32 netapi32 ole32 advapi32 dbghelp msvcrt kernel32)
add_rostests_file(TARGET credprov_harness)
