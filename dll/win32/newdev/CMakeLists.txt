
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl ${CMAKE_CURRENT_BINARY_DIR})
add_rpc_files(client ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/drvinst.idl)
spec2def(newdev.dll newdev.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    driver_cache.c
    newdev.c
    rollback.c
    stubs.c
    wizard.c
    driver_cache.h
    newdev_private.h
    ${CMAKE_CURRENT_BINARY_DIR}/newdev_stubs.c)

add_library(newdev MODULE
    ${SOURCE}
    drvinst_client.c
    ${CMAKE_CURRENT_BINARY_DIR}/drvinst_c.c
    newdev.rc
    ${CMAKE_CURRENT_BINARY_DIR}/newdev.def)

set_module_type(newdev win32dll UNICODE)
target_link_libraries(newdev wine ${PSEH_LIB})
add_importlibs(newdev gdi32 comctl32 setupapi advapi32 user32 shell32 shlwapi ole32 rpcrt4 msvcrt kernel32 ntdll)
add_pch(newdev newdev_private.h SOURCE)
add_cd_file(TARGET newdev DESTINATION reactos/system32 FOR all)
