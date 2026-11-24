
list(APPEND SOURCE
    main.c
    ipc.c
    ui.c
    logonuihost.h)

add_executable(logonuihost ${SOURCE} logonuihost.rc)
set_module_type(logonuihost win32gui UNICODE)
target_link_libraries(logonuihost wluiclient wine ${PSEH_LIB})
add_importlibs(logonuihost user32 gdi32 advapi32 rpcrt4 msvcrt kernel32 ntdll)
add_custom_command(TARGET logonuihost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DMODULE=$<TARGET_FILE:logonuihost> -P ${CMAKE_CURRENT_SOURCE_DIR}/nohooks.cmake)
add_cd_file(TARGET logonuihost DESTINATION reactos/system32 FOR all)

# Test build: the same sources plus the test-only switches; never installed in the image.
if(ENABLE_ROSTESTS)
    add_executable(logonuihost_test ${SOURCE} testhooks.c logonuihost.rc)
    set_module_type(logonuihost_test win32gui UNICODE)
    target_compile_definitions(logonuihost_test PRIVATE LOGONUI_TEST_HOOKS)
    target_link_libraries(logonuihost_test wluiclient wine ${PSEH_LIB})
    add_importlibs(logonuihost_test user32 gdi32 advapi32 rpcrt4 msvcrt kernel32 ntdll)
    add_rostests_file(TARGET logonuihost_test)
endif()
