# sihost.exe - Shell Infrastructure Host
# CMake build configuration (ReactOS-shaped)

set_module_type(sihost win32gui UNICODE)

target_sources(sihost PRIVATE sihost.cpp)

add_importlibs(sihost kernel32 ntdll)

add_cd_file(TARGET sihost DESTINATION system32 FOR all)
