
add_executable(slidetoshutdown slidetoshutdown.c slidetoshutdown.rc)
set_module_type(slidetoshutdown win32gui UNICODE)
target_link_libraries(slidetoshutdown luhgfx uuid wine)
add_importlibs(slidetoshutdown wtsapi32 ole32 user32 gdi32 winmm advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET slidetoshutdown DESTINATION reactos/system32 FOR all)
