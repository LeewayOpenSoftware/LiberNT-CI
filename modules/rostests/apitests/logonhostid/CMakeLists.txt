
list(APPEND SOURCE
    Identity.c
    testlist.c)

add_executable(logonhostid_apitest ${SOURCE})
set_module_type(logonhostid_apitest win32cui)
add_importlibs(logonhostid_apitest advapi32 user32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET logonhostid_apitest)
