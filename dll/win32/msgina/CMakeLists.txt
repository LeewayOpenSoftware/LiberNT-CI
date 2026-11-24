
spec2def(msgina.dll msgina.spec)

list(APPEND SOURCE
    gui.c
    lsa.c
    msgina.c
    shutdown.c
    stubs.c
    tui.c
    utils.c)

list(APPEND PCH_SKIP_SOURCE
    logonui.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/msgina_stubs.c)

add_library(msgina MODULE
    ${SOURCE}
    ${PCH_SKIP_SOURCE}
    msgina.rc
    ${CMAKE_CURRENT_BINARY_DIR}/msgina.def)

set_module_type(msgina win32dll UNICODE)
target_link_libraries(msgina wine uuid bootstatus shellframe ${PSEH_LIB} cpprt)

# LAB ONLY: the cover-post fault-injection hook (env MSGINA_COVER_FAIL_POST)
# must never ship in a default image. The option adds the compile definition;
# without it a post-build check fails the build if the hook string is present.
option(LIBERNT_MSGINA_TEST_HOOKS "LAB ONLY: enable the msgina cover-post fault injection hook" OFF)
if(LIBERNT_MSGINA_TEST_HOOKS)
    target_compile_definitions(msgina PRIVATE MSGINA_TEST_HOOKS=1)
else()
    add_custom_command(TARGET msgina POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DDLL=$<TARGET_FILE:msgina>
            -DNEEDLE=MSGINA_COVER_FAIL_POST
            -P ${CMAKE_CURRENT_SOURCE_DIR}/nohooks.cmake
        COMMENT "Checking msgina.dll is free of test-hook strings")
endif()
add_delay_importlibs(msgina secur32 netapi32 samlib gdiplus)
add_importlibs(msgina advapi32 user32 gdi32 userenv msvcrt kernel32 ntdll)
add_pch(msgina msgina.h "${PCH_SKIP_SOURCE}")
add_cd_file(TARGET msgina DESTINATION reactos/system32 FOR all)
