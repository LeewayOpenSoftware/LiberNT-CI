
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs)

list(APPEND SOURCE
    SChannelChain.c
    testlist.c)

add_executable(secur32_apitest ${SOURCE})
target_link_libraries(secur32_apitest wine ${PSEH_LIB})
set_module_type(secur32_apitest win32cui)
add_importlibs(secur32_apitest secur32 crypt32 mbedtls advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET secur32_apitest)
