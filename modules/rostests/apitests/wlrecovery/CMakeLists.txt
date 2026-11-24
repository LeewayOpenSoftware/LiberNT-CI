
list(APPEND SOURCE
    LevelMachine.c
    testlist.c)

add_executable(wlrecovery_apitest ${SOURCE})
target_link_libraries(wlrecovery_apitest wlrecovery wine)
set_module_type(wlrecovery_apitest win32cui)
add_importlibs(wlrecovery_apitest msvcrt kernel32 ntdll)
add_rostests_file(TARGET wlrecovery_apitest)
