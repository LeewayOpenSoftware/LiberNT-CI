# orosi-update.exe: command line of the Orosi Update stage 1 core.
# STANDALONE: not referenced by base/applications/cmdutils/CMakeLists.txt (see libservicing/CMakeLists.txt).

add_executable(orosi-update main.c)
set_module_type(orosi-update win32cui UNICODE)
target_link_libraries(orosi-update libservicing)
add_importlibs(orosi-update wintrust crypt32 advapi32 ws2_32 msvcrt kernel32)
# no add_cd_file(): the stage 1 core is not installed by the ISO
