
# Test-only: builds the host core with WPH_FAULT_INJECTION to force each exit
# code. Never installed (add_rostests_file), so production ships no injection code.

list(APPEND SOURCE
    wallpaperhost_faulttest.cpp
    ${REACTOS_SOURCE_DIR}/base/applications/wallpaperhost/wallpaperhost.cpp)

add_executable(wallpaperhost_faulttest ${SOURCE})
target_compile_definitions(wallpaperhost_faulttest PRIVATE WPH_FAULT_INJECTION)
target_include_directories(wallpaperhost_faulttest PRIVATE
    ${REACTOS_SOURCE_DIR}/base/applications/wallpaperhost)
target_link_libraries(wallpaperhost_faulttest cpprt uuid)
set_module_type(wallpaperhost_faulttest win32cui)
add_importlibs(wallpaperhost_faulttest msvcrt kernel32 user32 ole32 ntdll)
add_rostests_file(TARGET wallpaperhost_faulttest)
