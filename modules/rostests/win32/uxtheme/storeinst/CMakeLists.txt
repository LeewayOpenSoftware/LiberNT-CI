
add_executable(storeinst storeinst.c)
set_module_type(storeinst win32cui UNICODE)
add_importlibs(storeinst advapi32 user32 kernel32 msvcrt)
add_rostests_file(TARGET storeinst SUBDIR suppl)
