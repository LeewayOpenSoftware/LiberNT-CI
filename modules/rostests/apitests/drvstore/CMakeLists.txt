add_executable(drvstore_apitest
    drvstore.c
    testlist.c)

set_module_type(drvstore_apitest win32cui)
add_importlibs(drvstore_apitest setupapi user32 msvcrt kernel32)
add_rostests_file(TARGET drvstore_apitest)
