
add_executable(wevtutil wevtutil.c wevtutil.rc)
set_module_type(wevtutil win32cui UNICODE)
target_link_libraries(wevtutil conutils)
add_importlibs(wevtutil wevtapi advapi32 msvcrt kernel32)
add_cd_file(TARGET wevtutil DESTINATION reactos/system32 FOR all)
