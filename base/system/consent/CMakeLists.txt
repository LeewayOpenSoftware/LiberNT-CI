
add_executable(consent consent.c consent.rc)
set_module_type(consent win32gui UNICODE)
add_importlibs(consent user32 msvcrt kernel32)
add_cd_file(TARGET consent DESTINATION reactos/system32 FOR all)
