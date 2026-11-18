
list(APPEND SOURCE
    wevtutil.c
    testlist.c)

add_executable(wevtutil_apitest ${SOURCE})
set_module_type(wevtutil_apitest win32cui)
add_importlibs(wevtutil_apitest msvcrt kernel32)
add_rostests_file(TARGET wevtutil_apitest)
