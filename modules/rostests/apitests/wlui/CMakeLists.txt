
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

# The test is the server and links the real client: its manager routines carry a prefix so the
# generated client stubs stay in the link instead of being shadowed by same-named definitions.
add_rpc_file(server ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl PREFIX_SERVER Test)

list(APPEND SOURCE
    Ipc.c
    testlist.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_s.c)

add_executable(wlui_apitest ${SOURCE})
target_link_libraries(wlui_apitest wluiclient wluiauth wine)
set_module_type(wlui_apitest win32cui)
add_importlibs(wlui_apitest rpcrt4 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET wlui_apitest)
