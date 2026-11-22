# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
add_executable(mmdurab mmdurab.c)
set_module_type(mmdurab win32cui UNICODE)
add_importlibs(mmdurab msvcrt kernel32)
add_rostests_file(TARGET mmdurab)
