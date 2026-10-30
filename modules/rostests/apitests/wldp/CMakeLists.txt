
list(APPEND SOURCE
    WldpPolicy.c
    testlist.c)

add_executable(wldp_apitest ${SOURCE})
set_module_type(wldp_apitest win32cui)
add_importlibs(wldp_apitest shlwapi ole32 msvcrt kernel32)
add_rostests_file(TARGET wldp_apitest)
