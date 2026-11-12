# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
add_executable(lowil lowil.c)
set_module_type(lowil win32cui ENTRYPOINT mainCRTStartup)
add_importlibs(lowil advapi32 msvcrt kernel32)
add_rostests_file(TARGET lowil)
