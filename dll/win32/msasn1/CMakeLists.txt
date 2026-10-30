
add_definitions(-D__WINESRC__)
spec2def(msasn1.dll msasn1.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/msasn1_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/msasn1.def)

add_library(msasn1 MODULE ${SOURCE})
set_module_type(msasn1 win32dll)
target_link_libraries(msasn1 wine)
add_importlibs(msasn1 msvcrt kernel32 ntdll)
add_cd_file(TARGET msasn1 DESTINATION reactos/system32 FOR all)
