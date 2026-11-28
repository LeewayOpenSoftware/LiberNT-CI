
add_executable(sihost_apitest sihost.c testlist.c)
set_module_type(sihost_apitest win32cui)
add_importlibs(sihost_apitest user32 msvcrt kernel32)
add_rostests_file(TARGET sihost_apitest)
