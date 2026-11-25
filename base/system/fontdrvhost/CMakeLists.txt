
add_executable(fontdrvhost fontdrvhost.c fontdrvhost.rc)
set_module_type(fontdrvhost win32gui UNICODE)
target_link_libraries(fontdrvhost freetype ${PSEH_LIB})
if(UMFD_TEST_HOOKS)
    target_compile_definitions(fontdrvhost PRIVATE UMFD_TEST_HOOKS)
endif()
add_importlibs(fontdrvhost win32u msvcrt kernel32 ntdll)
add_cd_file(TARGET fontdrvhost DESTINATION reactos/system32 FOR all)
