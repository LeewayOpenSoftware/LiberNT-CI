# Copyright 2026 Leeway Open Software
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(mi_apitest MiEnumerate.c MiPush.c testlist.c)
set_module_type(mi_apitest win32cui)
target_link_libraries(mi_apitest wine uuid)
add_importlibs(mi_apitest mi oleaut32 ole32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET mi_apitest)
