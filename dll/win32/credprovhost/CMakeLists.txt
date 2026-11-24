
spec2def(credprovhost.dll credprovhost.spec ADD_IMPORTLIB)

add_library(credprovhost MODULE
    credprovhost.c
    credprovhost.rc
    ${CMAKE_CURRENT_BINARY_DIR}/credprovhost.def)

set_module_type(credprovhost win32dll)
target_link_libraries(credprovhost uuid wine)
add_importlibs(credprovhost ole32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET credprovhost DESTINATION reactos/system32 FOR all)
