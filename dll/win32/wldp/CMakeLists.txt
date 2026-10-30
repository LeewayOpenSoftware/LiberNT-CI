
spec2def(wldp.dll wldp.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    wldp.c
    ${CMAKE_CURRENT_BINARY_DIR}/wldp_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/wldp.def)

add_library(wldp MODULE ${SOURCE})
set_module_type(wldp win32dll ENTRYPOINT 0)
target_link_libraries(wldp uuid)
add_importlibs(wldp kernel32 ntdll)
add_cd_file(TARGET wldp DESTINATION reactos/system32 FOR all)
