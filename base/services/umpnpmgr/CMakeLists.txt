
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl)
add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/pnp.idl ${REACTOS_SOURCE_DIR}/sdk/include/wine/plugplay.idl ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/drvinst.idl)
spec2def(umpnpmgr.dll umpnpmgr.spec ADD_IMPORTLIB)

add_library(umpnpmgr MODULE
    drvinst/job.c
    drvinst/rpc.c
    drvinst/rollback.c
    event.c
    install.c
    listener.c
    rpcserver.c
    umpnpmgr.c
    umpnpmgr.rc
    ${CMAKE_CURRENT_BINARY_DIR}/pnp_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/plugplay_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/drvinst_s.c
    ${CMAKE_CURRENT_BINARY_DIR}/umpnpmgr_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/umpnpmgr.def)

target_link_libraries(umpnpmgr wdmguid ${PSEH_LIB})
set_module_type(umpnpmgr win32dll UNICODE)
add_importlibs(umpnpmgr advapi32 cfgmgr32 setupapi rpcrt4 userenv shlwapi msvcrt user32 kernel32 ntdll)
add_cd_file(TARGET umpnpmgr DESTINATION reactos/system32 FOR all)
