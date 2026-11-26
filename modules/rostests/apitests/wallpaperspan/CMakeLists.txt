
add_executable(wallpaperspan_apitest
    SpanGeometry.c
    testlist.c
    ${REACTOS_SOURCE_DIR}/win32ss/user/ntuser/wallpaperspan.c)
target_include_directories(wallpaperspan_apitest PRIVATE ${REACTOS_SOURCE_DIR}/win32ss/user/ntuser)
set_module_type(wallpaperspan_apitest win32cui)
add_importlibs(wallpaperspan_apitest msvcrt kernel32)
add_rostests_file(TARGET wallpaperspan_apitest)
