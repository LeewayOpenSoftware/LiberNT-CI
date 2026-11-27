# Copyright 2026 LeewayOpenSoftware
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(tdh_apitest TdhTl.c testlist.c)
set_module_type(tdh_apitest win32cui)
target_link_libraries(tdh_apitest wine)
add_importlibs(tdh_apitest tdh kernel32 ntdll)
add_rostests_file(TARGET tdh_apitest)
