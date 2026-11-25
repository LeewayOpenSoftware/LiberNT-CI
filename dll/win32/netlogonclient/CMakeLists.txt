include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl)

add_rpc_files(client
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/netlogon.idl)

add_library(netlogonclient STATIC
    dcopen.c
    netlogon.c
    rpcmem.c
    ${CMAKE_CURRENT_BINARY_DIR}/netlogon_c.c)

target_compile_definitions(netlogonclient PRIVATE WIN32_LEAN_AND_MEAN)
add_dependencies(netlogonclient psdk)
