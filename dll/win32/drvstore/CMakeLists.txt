
spec2def(drvstore.dll drvstore.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/drvstore_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/drvstore.def)

add_library(drvstore MODULE ${SOURCE})
set_module_type(drvstore win32dll ENTRYPOINT 0)
add_importlibs(drvstore kernel32 ntdll)
add_cd_file(TARGET drvstore DESTINATION reactos/system32 FOR all)
