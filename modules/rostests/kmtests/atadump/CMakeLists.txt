include_directories(../include)

list(APPEND ATADUMP_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    atadump_drv.c)

add_library(atadump_drv MODULE ${ATADUMP_DRV_SOURCE})
set_module_type(atadump_drv kernelmodedriver)
target_link_libraries(atadump_drv kmtest_printf ${PSEH_LIB})
add_importlibs(atadump_drv ntoskrnl hal)
target_compile_definitions(atadump_drv PRIVATE KMT_STANDALONE_DRIVER)
add_rostests_file(TARGET atadump_drv)
