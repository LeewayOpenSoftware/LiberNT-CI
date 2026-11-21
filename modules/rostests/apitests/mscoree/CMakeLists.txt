
list(APPEND SOURCE
    mscoree.c
    testlist.c)

add_executable(mscoree_apitest ${SOURCE})
target_link_libraries(mscoree_apitest wine uuid ${PSEH_LIB})
set_module_type(mscoree_apitest win32cui)
add_importlibs(mscoree_apitest version msvcrt kernel32 ntdll)
add_rostests_file(TARGET mscoree_apitest)
