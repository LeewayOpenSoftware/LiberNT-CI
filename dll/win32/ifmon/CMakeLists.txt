
spec2def(ifmon.dll ifmon.spec)

list(APPEND SOURCE
    guid.c
    ifmon.c
    ifmon.rc
    interface.c
    ip.c
    ipv6.c
    winsock.c
    ${CMAKE_CURRENT_BINARY_DIR}/ifmon.def)

add_library(ifmon MODULE ${SOURCE})
set_module_type(ifmon win32dll UNICODE)
target_link_libraries(ifmon uuid)
add_importlibs(ifmon netsh ole32 advapi32 iphlpapi ws2_32 msvcrt kernel32 ntdll)
add_cd_file(TARGET ifmon DESTINATION reactos/system32 FOR all)
