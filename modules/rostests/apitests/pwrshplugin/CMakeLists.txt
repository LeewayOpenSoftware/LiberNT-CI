
list(APPEND SOURCE
    pwrshplugin.c
    testlist.c)

add_executable(pwrshplugin_apitest ${SOURCE})
target_link_libraries(pwrshplugin_apitest wine ${PSEH_LIB})
set_module_type(pwrshplugin_apitest win32cui)
add_importlibs(pwrshplugin_apitest version msvcrt kernel32 ntdll)
add_rostests_file(TARGET pwrshplugin_apitest)
