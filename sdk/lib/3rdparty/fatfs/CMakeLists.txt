add_library(fatfs STATIC
    ff.c
    ffunicode.c)
set_bootloader_optimization(fatfs)

target_include_directories(fatfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fatfs PUBLIC FF_USE_REACTOS_TYPES)
add_dependencies(fatfs xdk)
