# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
add_executable(elevadm elevadm.c elevadm.rc)
set_module_type(elevadm win32cui ENTRYPOINT mainCRTStartup)
add_importlibs(elevadm advapi32 msvcrt kernel32)
add_rostests_file(TARGET elevadm)
