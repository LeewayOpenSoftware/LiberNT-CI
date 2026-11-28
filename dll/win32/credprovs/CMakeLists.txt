
spec2def(credprovs.dll credprovs.spec)

list(APPEND SOURCE
    credential.c
    main.c
    provider.c)

add_library(credprovs MODULE
    ${SOURCE}
    samquery.c
    credprovs.rc
    ${CMAKE_CURRENT_BINARY_DIR}/credprovs.def)

set_module_type(credprovs win32dll)
target_link_libraries(credprovs credser uuid wine wine_dll_register)
add_importlibs(credprovs secur32 ole32 user32 advapi32 samlib msvcrt kernel32 ntdll)
add_pch(credprovs precomp.h SOURCE)
add_custom_command(TARGET credprovs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DMODULE=$<TARGET_FILE:credprovs> -P ${CMAKE_CURRENT_SOURCE_DIR}/nohooks.cmake)
add_cd_file(TARGET credprovs DESTINATION reactos/system32 FOR all)

if(ENABLE_ROSTESTS)
    spec2def(credprovs_test.dll credprovs_test.spec)
    add_library(credprovs_test MODULE
        ${SOURCE}
        samquery.c
        testhooks.c
        credprovs.rc
        ${CMAKE_CURRENT_BINARY_DIR}/credprovs_test.def)
    set_module_type(credprovs_test win32dll)
    target_compile_definitions(credprovs_test PRIVATE CREDPROVS_TEST_HOOKS)
    target_link_libraries(credprovs_test credser uuid wine wine_dll_register)
    add_importlibs(credprovs_test secur32 ole32 user32 advapi32 samlib msvcrt kernel32 ntdll)
    add_rostests_file(TARGET credprovs_test)
endif()
