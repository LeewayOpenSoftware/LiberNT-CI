# The kernelbase functions moved from kernel32: NDK code, built without the Wine
# definitions and include path of the parent directory.
add_definitions(-D_KERNELBASE_)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys)

list(APPEND SOURCE
    ComputerName.c
    debugger.c
    Environment.c
    FileMapping.c
    SystemInfo.c
    VirtualMemory.c)

add_library(kernelbase_nt ${SOURCE})
target_link_libraries(kernelbase_nt PRIVATE pseh)
add_dependencies(kernelbase_nt psdk)
