
list(APPEND SOURCE
    cmdline.c
    wlrmdr.c
    wlrmdr.h)

add_executable(wlrmdr ${SOURCE} wlrmdr.rc)
set_module_type(wlrmdr win32gui UNICODE)
add_importlibs(wlrmdr user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET wlrmdr DESTINATION reactos/system32 FOR all)
