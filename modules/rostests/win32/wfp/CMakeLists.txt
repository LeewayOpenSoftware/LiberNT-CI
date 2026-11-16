
add_executable(wfpmode wfpmode.c)
set_module_type(wfpmode win32cui UNICODE)
add_importlibs(wfpmode fwpuclnt ws2_32 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wfpmode)
