
add_subdirectory(harness)
add_subdirectory(testmod)

list(APPEND SOURCE
    Abi.c
    CredUIPack.c
    Framework.c
    PasswordProvider.c
    Serialization.c
    testlist.c)

add_executable(credprov_apitest ${SOURCE})
set_module_type(credprov_apitest win32cui)
target_link_libraries(credprov_apitest credser uuid)
add_importlibs(credprov_apitest ole32 advapi32 secur32 msvcrt kernel32)
add_rostests_file(TARGET credprov_apitest)
