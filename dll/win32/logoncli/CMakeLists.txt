
spec2def(logoncli.dll logoncli.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    logoncli_main.c
    ${CMAKE_CURRENT_BINARY_DIR}/logoncli.def)

add_library(logoncli MODULE ${SOURCE})
set_module_type(logoncli win32dll ENTRYPOINT DllMain 12)
target_link_libraries(logoncli wine ${PSEH_LIB} netlogonclient)
add_importlibs(logoncli netutils rpcrt4 msvcrt kernel32 ntdll)
add_cd_file(TARGET logoncli DESTINATION reactos/system32 FOR all)
