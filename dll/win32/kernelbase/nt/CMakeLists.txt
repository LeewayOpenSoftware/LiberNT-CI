# The kernelbase functions moved from kernel32: NDK code, built without the Wine
# definitions and include path of the parent directory.
add_definitions(-D_KERNELBASE_)
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/subsys)

list(APPEND SOURCE
    ComputerName.c
    debugger.c
    FileMapping.c
    SystemInfo.c
    VirtualMemory.c)

# amd64 raises from assembly so that the context keeps the caller's nonvolatile registers
if(ARCH STREQUAL "amd64")
    list(APPEND ASM_SOURCE
        amd64/except.S)
else()
    list(APPEND SOURCE
        raise.c)
endif()

add_asm_files(kernelbase_nt_asm ${ASM_SOURCE})
add_library(kernelbase_nt ${SOURCE} ${kernelbase_nt_asm})
target_link_libraries(kernelbase_nt PRIVATE pseh)
add_dependencies(kernelbase_nt psdk asm)
