
add_executable(fontdrvhost_apitest fontdrvhost.c testlist.c)
set_module_type(fontdrvhost_apitest win32cui)
add_importlibs(fontdrvhost_apitest advapi32 user32 msvcrt kernel32)
add_rostests_file(TARGET fontdrvhost_apitest)
