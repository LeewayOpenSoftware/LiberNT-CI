# Drop the inherited global target version before bumping to 0xA01.
remove_definitions(-D_WIN32_WINNT=${REACTOS_TARGET_NT} -DWINVER=${REACTOS_TARGET_NT})
add_definitions(-D_WIN32_WINNT=0xA01 -DWINVER=0xA01)

add_definitions(
    -D__WINESRC__
    -D_WINE
    -Dwcsnicmp=_wcsnicmp
    -Dwcsicmp=_wcsicmp
    -Dstrnicmp=_strnicmp
    -D_KERNEL32_
    -D_KERNELBASE_)

include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Silence GCC/Clang warnings
    add_compile_options(
        -Wno-unused-function
        -Wno-unknown-pragmas
        -Wno-unused-variable
        -Wno-unused-value
        -Wno-unused-function
    )
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder>)
endif()

#list(APPEND SOURCE_PATH
#    path.c
#    )
#
#add_library(kernelbase_path ${SOURCE_PATH})
#target_link_libraries(kernelbase_path ${PSEH_LIB})
#add_dependencies(kernelbase_path psdk)

if(NOT MSVC)
    #target_compile_options(kernelbase_path PRIVATE -Wno-unused-variable)
    #target_compile_options(kernelbase_path PRIVATE -Wno-unused-function)
endif()

list(APPEND SOURCE_STATIC
    console.c
    debug.c
    locale.c
    main.c
    path.c
    registry.c
    security.c
    string.c
    thread.c
    version.c
    )

if(MSVC AND ARCH STREQUAL "amd64")
    add_asm_files(kernelbase_static_asm amd64/fiber.s)
    list(APPEND SOURCE_STATIC ${kernelbase_static_asm})
elseif(MSVC AND ARCH STREQUAL "i386")
    add_asm_files(kernelbase_static_asm i386/fiber.s)
    list(APPEND SOURCE_STATIC ${kernelbase_static_asm})
elseif(MSVC AND ARCH STREQUAL "arm64")
    add_asm_files(kernelbase_static_asm arm64/fiber.s)
    list(APPEND SOURCE_STATIC ${kernelbase_static_asm})
endif()

add_library(kernelbase_static ${SOURCE_STATIC})
target_compile_definitions(kernelbase_static PRIVATE WINSHLWAPI= WINPATHCCHAPI= PATHCCH_NO_DEPRECATE=)
target_link_libraries(kernelbase_static psapi_static ${PSEH_LIB})
add_dependencies(kernelbase_static psdk)

if(MSVC)
    target_compile_options(kernelbase_static PRIVATE
        /wd4267 # warning C4267: '=': conversion from 'size_t' to 'unsigned int', possible loss of data
        /wd4146 # warning C4146: unary minus operator applied to unsigned type, result still unsigned
    )
else()
    target_compile_options(kernelbase_static PRIVATE -Wno-unused-variable)
    target_compile_options(kernelbase_static PRIVATE -Wno-unused-function)
endif()

# Tramo D, Frente 4 (G1-G2; volume.c stays out until the mount manager question is settled): Wine bodies not yet exported. Built as separate
# static libraries so they are compiled and checked but NOT linked into
# kernelbase.dll; a function moves to the export table only with a proof.
add_library(kernelbase_wine_g1 sync.c)
target_compile_definitions(kernelbase_wine_g1 PRIVATE WINSHLWAPI= WINPATHCCHAPI= PATHCCH_NO_DEPRECATE=)
target_link_libraries(kernelbase_wine_g1 psapi_static ${PSEH_LIB})
add_dependencies(kernelbase_wine_g1 psdk)

add_library(kernelbase_wine_g2 memory.c)
target_compile_definitions(kernelbase_wine_g2 PRIVATE WINSHLWAPI= WINPATHCCHAPI= PATHCCH_NO_DEPRECATE=)
target_link_libraries(kernelbase_wine_g2 psapi_static ${PSEH_LIB})
add_dependencies(kernelbase_wine_g2 psdk)

if(NOT MSVC)
    foreach(_t kernelbase_wine_g1 kernelbase_wine_g2)
        target_compile_options(${_t} PRIVATE -Wno-unused-variable -Wno-unused-function)
    endforeach()
endif()
