
include_directories(
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl
    ${CMAKE_CURRENT_BINARY_DIR})

add_rpc_files(client ${REACTOS_SOURCE_DIR}/sdk/include/reactos/idl/wlui.idl)

list(APPEND SOURCE
    wluiclient.c
    wluiclient.h
    ${CMAKE_CURRENT_BINARY_DIR}/wlui_c.c)

add_library(wluiclient ${SOURCE})
target_include_directories(wluiclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wluiclient ${PSEH_LIB})
add_dependencies(wluiclient psdk)
