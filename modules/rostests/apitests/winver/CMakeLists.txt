
list(APPEND SOURCE
    winver.c
    testlist.c
    ${REACTOS_SOURCE_DIR}/base/applications/winver/brand.c)

add_executable(winver_apitest ${SOURCE})
target_link_libraries(winver_apitest wine uuid ${PSEH_LIB})
set_module_type(winver_apitest win32cui)
add_importlibs(winver_apitest msvcrt kernel32 ntdll)
add_rostests_file(TARGET winver_apitest)
