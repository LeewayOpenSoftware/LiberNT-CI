
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

add_rpc_file(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl PREFIX_SERVER Test)

list(APPEND SOURCE
    Recovery.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_s.c)

add_executable(logonrecovery_apitest ${SOURCE})
target_link_libraries(logonrecovery_apitest wluiclient wluiauth credser wine)
set_module_type(logonrecovery_apitest win32cui)
add_importlibs(logonrecovery_apitest rpcrt4 advapi32 user32 msvcrt kernel32 ntdll)
add_dependencies(logonrecovery_apitest logonrecovery_test)
add_rostests_file(TARGET logonrecovery_apitest)
