
add_executable(themetest themetest.c)
set_module_type(themetest win32cui UNICODE)
add_importlibs(themetest uxtheme gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET themetest SUBDIR suppl)
