
spec2def(mi.dll mi.spec ADD_IMPORTLIB)

add_library(mi MODULE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/mi.def)

set_module_type(mi win32dll)
target_link_libraries(mi uuid oldnames)
add_importlibs(mi oleaut32 ole32 msvcrt kernel32 ntdll)
add_cd_file(TARGET mi DESTINATION reactos/system32 FOR all)
