
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

add_rpc_files(server
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/winreg.idl
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl)

list(APPEND SOURCE
    environment.c
    hostlaunch.c
    hostlogon.c
    notify.c
    rpclogonui.c
    rpcserver.c
    sas.c
    screensaver.c
    security.c
    setup.c
    shutdown.c
    umfdlaunch.c
    winlogon.c
    wlx.c
    winlogon.h
    ${CMAKE_CURRENT_BINARY_DIR}/winreg_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_s.c)

add_rc_deps(winlogon.rc ${CMAKE_CURRENT_SOURCE_DIR}/res/winlogon.ico)
add_executable(winlogon ${SOURCE} winlogon.rc)
target_link_libraries(winlogon wine win32ksys wluiauth wlimgcheck wlrecovery credser uuid ${PSEH_LIB})
set_module_type(winlogon win32gui)
add_importlibs(winlogon user32 advapi32 userenv secur32 rpcrt4 mpr ole32 msvcrt kernel32 ntdll)
add_pch(winlogon winlogon.h SOURCE)
add_cd_file(TARGET winlogon DESTINATION reactos/system32 FOR all)
