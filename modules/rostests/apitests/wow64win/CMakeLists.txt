
add_executable(wow64win_apitest GdiOnlyCallback.c testlist.c)
set_module_type(wow64win_apitest win32cui)
add_importlibs(wow64win_apitest win32u msvcrt kernel32)
add_rostests_file(TARGET wow64win_apitest)
