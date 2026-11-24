
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

add_rpc_files(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl)

list(APPEND SOURCE
    Ipc.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_s.c)

add_executable(wlui_apitest ${SOURCE})
target_link_libraries(wlui_apitest wluiclient wine)
set_module_type(wlui_apitest win32cui)
add_importlibs(wlui_apitest rpcrt4 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wlui_apitest)
