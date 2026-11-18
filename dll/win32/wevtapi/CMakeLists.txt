
add_definitions(-D__WINESRC__)
spec2def(wevtapi.dll wevtapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    publisher.c
    query.c
    render.c
    subscribe.c
    ${CMAKE_CURRENT_BINARY_DIR}/wevtapi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/wevtapi.def)

add_library(wevtapi MODULE ${SOURCE})
set_module_type(wevtapi win32dll)
target_link_libraries(wevtapi wine)
add_importlibs(wevtapi advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET wevtapi DESTINATION reactos/system32 FOR all)
set_wine_module(wevtapi)

if(MSVC)
    remove_target_compile_option(wevtapi "/we4312")
endif()
