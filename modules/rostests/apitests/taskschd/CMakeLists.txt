
list(APPEND SOURCE
    TaskEngine.c
    testlist.c)

add_executable(taskschd_apitest ${SOURCE})
set_module_type(taskschd_apitest win32cui)
add_importlibs(taskschd_apitest ole32 oleaut32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET taskschd_apitest)
