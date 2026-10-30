
spec2def(devobj.dll devobj.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/devobj_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/devobj.def)

add_library(devobj MODULE ${SOURCE})
set_module_type(devobj win32dll ENTRYPOINT 0)
add_importlibs(devobj kernel32 ntdll)
add_cd_file(TARGET devobj DESTINATION reactos/system32 FOR all)
