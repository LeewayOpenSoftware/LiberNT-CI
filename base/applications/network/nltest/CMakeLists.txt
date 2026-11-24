
include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/conutils)

add_executable(nltest nltest.c nltest.rc)
set_module_type(nltest win32cui UNICODE)
target_link_libraries(nltest conutils ${PSEH_LIB})
# logoncli first: the Netlogon client entry points resolve from it, as on Windows
add_importlibs(nltest logoncli netapi32 advapi32 rpcrt4 ws2_32 msvcrt kernel32 ntdll)
add_cd_file(TARGET nltest DESTINATION reactos/system32 FOR all)
