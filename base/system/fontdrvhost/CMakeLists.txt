
add_executable(fontdrvhost fontdrvhost.c fontdrvhost.rc)
set_module_type(fontdrvhost win32gui UNICODE)
add_importlibs(fontdrvhost kernel32 ntdll)
add_cd_file(TARGET fontdrvhost DESTINATION reactos/system32 FOR all)
