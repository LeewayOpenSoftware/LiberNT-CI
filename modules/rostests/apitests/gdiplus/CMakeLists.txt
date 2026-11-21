add_executable(gdiplus_apitest InstalledCollection.c testlist.c)
set_module_type(gdiplus_apitest win32cui)
add_importlibs(gdiplus_apitest gdiplus user32 gdi32 msvcrt kernel32)
add_rostests_file(TARGET gdiplus_apitest)
