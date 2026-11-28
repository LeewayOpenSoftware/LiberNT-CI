
add_executable(setup setup.c setup.rc)
set_module_type(setup win32gui UNICODE ENTRYPOINT wmainCRTStartup)
add_importlibs(setup userenv user32 msvcrt kernel32 ntdll)
add_cd_file(TARGET setup DESTINATION reactos/system32/oobe FOR all)
