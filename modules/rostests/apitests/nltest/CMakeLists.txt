
list(APPEND SOURCE
    nltest.c
    testlist.c)

add_executable(nltest_apitest ${SOURCE})
set_module_type(nltest_apitest win32cui)
add_importlibs(nltest_apitest netapi32 advapi32 msvcrt kernel32)
add_rostests_file(TARGET nltest_apitest)
