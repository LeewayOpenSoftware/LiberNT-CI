
list(APPEND SOURCE
    cmdline.c
    process.c
    testlist.c
    ${CMAKE_SOURCE_DIR}/base/system/wlrmdr/cmdline.c)

add_executable(wlrmdr_apitest ${SOURCE})
target_include_directories(wlrmdr_apitest PRIVATE ${CMAKE_SOURCE_DIR}/base/system/wlrmdr)
set_module_type(wlrmdr_apitest win32cui)
add_importlibs(wlrmdr_apitest user32 msvcrt kernel32)
add_rostests_file(TARGET wlrmdr_apitest)
