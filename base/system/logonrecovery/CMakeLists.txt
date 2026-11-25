
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos)

list(APPEND SOURCE
    main.c
    logonrecovery.h)

# Closed dependency set (PHASE3-PLAN 8.11 a): kernel32, user32, gdi32, rpcrt4 and the static
# credser and wlui client; no COM, no credprovs.dll, no GDI+. The VM leg checks the module list.
add_executable(logonrecovery ${SOURCE} logonrecovery.rc)
set_module_type(logonrecovery win32gui UNICODE)
target_link_libraries(logonrecovery wluiclient credser wine ${PSEH_LIB})
add_importlibs(logonrecovery user32 gdi32 rpcrt4 msvcrt kernel32 ntdll)
add_custom_command(TARGET logonrecovery POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DMODULE=$<TARGET_FILE:logonrecovery> -P ${CMAKE_CURRENT_SOURCE_DIR}/nohooks.cmake)
add_cd_file(TARGET logonrecovery DESTINATION reactos/system32 FOR all)

# Test build: the same sources plus the test-only switches; never installed in the image.
if(ENABLE_ROSTESTS)
    add_executable(logonrecovery_test ${SOURCE} testhooks.c logonrecovery.rc)
    set_module_type(logonrecovery_test win32gui UNICODE)
    target_compile_definitions(logonrecovery_test PRIVATE LOGONUI_TEST_HOOKS)
    target_link_libraries(logonrecovery_test wluiclient credser wine ${PSEH_LIB})
    add_importlibs(logonrecovery_test advapi32 user32 gdi32 rpcrt4 msvcrt kernel32 ntdll)
    add_rostests_file(TARGET logonrecovery_test)
endif()
