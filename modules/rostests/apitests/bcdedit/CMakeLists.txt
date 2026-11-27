
list(APPEND SOURCE
    BcdEdit.c
    testlist.c)

add_executable(bcdedit_apitest ${SOURCE})
target_link_libraries(bcdedit_apitest ${PSEH_LIB})
set_module_type(bcdedit_apitest win32cui)
add_importlibs(bcdedit_apitest advapi32 msvcrt kernel32)
add_rostests_file(TARGET bcdedit_apitest)
