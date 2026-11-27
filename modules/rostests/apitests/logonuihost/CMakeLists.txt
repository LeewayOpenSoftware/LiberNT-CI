
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

add_rpc_file(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl PREFIX_SERVER Test)

list(APPEND SOURCE
    Host.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_s.c)

add_executable(logonuihost_apitest ${SOURCE})
target_link_libraries(logonuihost_apitest wluiclient wluiauth uuid wine)
set_module_type(logonuihost_apitest win32cui)
add_importlibs(logonuihost_apitest rpcrt4 ole32 advapi32 user32 msvcrt kernel32 ntdll)
add_dependencies(logonuihost_apitest logonuihost_test credprov_testmod)
add_rostests_file(TARGET logonuihost_apitest)
