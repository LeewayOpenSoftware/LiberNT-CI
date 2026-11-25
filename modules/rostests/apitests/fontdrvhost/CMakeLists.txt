
add_executable(fontdrvhost_apitest fontdrvhost.c umfd_channel.c testlist.c)
set_module_type(fontdrvhost_apitest win32cui)
if(UMFD_TEST_HOOKS)
    target_compile_definitions(fontdrvhost_apitest PRIVATE UMFD_TEST_HOOKS)
endif()
add_importlibs(fontdrvhost_apitest advapi32 secur32 user32 gdi32 win32u msvcrt kernel32)
add_rostests_file(TARGET fontdrvhost_apitest)
