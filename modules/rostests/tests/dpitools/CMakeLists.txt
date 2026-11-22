# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
add_executable(dpidump dpidump.c dpitools.rc)
set_module_type(dpidump win32cui ENTRYPOINT mainCRTStartup)
add_importlibs(dpidump comctl32 uxtheme shell32 advapi32 gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET dpidump)

add_executable(dpishot dpishot.c dpitools.rc)
set_module_type(dpishot win32gui UNICODE)
add_importlibs(dpishot comctl32 shell32 advapi32 gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET dpishot)
