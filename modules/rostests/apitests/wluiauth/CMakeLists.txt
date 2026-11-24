
list(APPEND SOURCE
    Authorize.c
    LaunchState.c
    testlist.c)

add_executable(wluiauth_apitest ${SOURCE})
target_link_libraries(wluiauth_apitest wluiauth wine)
set_module_type(wluiauth_apitest win32cui)
add_importlibs(wluiauth_apitest rpcrt4 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wluiauth_apitest)
