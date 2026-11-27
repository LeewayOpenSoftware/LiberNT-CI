#
# PROJECT:     LiberNT Boot Configuration Data editor
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Builds the bcdedit command-line tool
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#

add_executable(bcdedit bcdedit.c bcdedit.rc)
set_module_type(bcdedit win32cui UNICODE)
add_importlibs(bcdedit advapi32 user32 msvcrt kernel32)
add_cd_file(TARGET bcdedit DESTINATION reactos/system32 FOR all)
