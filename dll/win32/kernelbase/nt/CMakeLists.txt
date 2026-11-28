# The kernelbase functions moved from kernel32: NDK code, built without the Wine
# definitions and include path of the parent directory.
add_definitions(-D_KERNELBASE_)

list(APPEND SOURCE
    FileMapping.c
    VirtualMemory.c)

add_library(kernelbase_nt ${SOURCE})
target_link_libraries(kernelbase_nt PRIVATE pseh)
add_dependencies(kernelbase_nt psdk)
