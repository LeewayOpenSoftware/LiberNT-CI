
add_executable(dllhost dllhost.c dllhost.rc)
target_link_libraries(dllhost uuid wine)
set_module_type(dllhost win32gui UNICODE)
add_importlibs(dllhost ole32 msvcrt kernel32 ntdll)
add_cd_file(TARGET dllhost DESTINATION reactos/system32 FOR all)
