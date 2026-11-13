spec2def(twinapi.appcore.dll twinapi.appcore.spec)
add_idl_headers(twinapi_appcore_private_idl async_private.idl)

list(APPEND SOURCE
    async.c
    main.c
    map.c)

add_library(twinapi_appcore MODULE ${SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/twinapi.appcore.def ${CMAKE_CURRENT_BINARY_DIR}/twinapi.appcore_stubs.c)
set_target_properties(twinapi_appcore PROPERTIES OUTPUT_NAME "twinapi.appcore")
add_idl_reg_scripts(twinapi_appcore winrt classes.idl)
add_dependencies(twinapi_appcore twinapi_appcore_private_idl)
set_module_type(twinapi_appcore win32dll)
target_compile_definitions(twinapi_appcore PRIVATE __WINESRC__)
target_link_libraries(twinapi_appcore uuid wine wine_dll_canunload wine_dll_register)
add_importlibs(twinapi_appcore combase advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET twinapi_appcore DESTINATION reactos/system32 FOR all)
set_wine_module(twinapi_appcore)
