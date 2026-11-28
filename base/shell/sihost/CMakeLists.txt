
add_executable(sihost sihost.c sihost.rc)
set_module_type(sihost win32gui UNICODE)
add_importlibs(sihost msvcrt kernel32 ntdll)
add_cd_file(TARGET sihost DESTINATION reactos/system32 FOR all)
