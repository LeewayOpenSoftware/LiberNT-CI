
list(APPEND SOURCE
    ImageCheck.c
    testlist.c)

add_executable(wlimgcheck_apitest ${SOURCE})
target_link_libraries(wlimgcheck_apitest wlimgcheck wine)
set_module_type(wlimgcheck_apitest win32cui)
add_importlibs(wlimgcheck_apitest advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wlimgcheck_apitest)
