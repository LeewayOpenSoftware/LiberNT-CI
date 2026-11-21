
spec2def(pwrshplugin.dll pwrshplugin.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    pwrshplugin.c
    ${CMAKE_CURRENT_BINARY_DIR}/pwrshplugin.def)

add_library(pwrshplugin MODULE ${SOURCE} pwrshplugin.rc)
set_module_type(pwrshplugin win32dll)
target_link_libraries(pwrshplugin oldnames)
add_importlibs(pwrshplugin kernel32 ntdll)
add_cd_file(TARGET pwrshplugin DESTINATION reactos/system32 FOR all)
