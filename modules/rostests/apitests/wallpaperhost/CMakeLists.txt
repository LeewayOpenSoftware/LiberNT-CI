
# Deterministic exit-code coverage for wallpaperhost.exe. The host core
# (base/applications/wallpaperhost/wallpaperhost.cpp) is compiled here with
# WPH_FAULT_INJECTION so each failure path can be forced without real COM.
# The production wallpaperhost target never defines that macro, so the shipped
# binary contains none of the injection code (credprovs_test.dll discipline).
# This module is a test artifact (add_rostests_file) and is never installed to
# the system image.

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
