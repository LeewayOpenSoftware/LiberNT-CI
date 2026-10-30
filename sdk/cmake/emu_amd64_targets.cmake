# PROJECT:     LiberNT Build System
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Define the AMD64 guest runtime shipped beside the AMD64 emulator
# COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>

include("${CMAKE_CURRENT_LIST_DIR}/compat_runtime_targets.cmake")

set(EMU_AMD64_MODULES ${COMPAT_RUNTIME_MODULES}
    ntdll)
set(EMU_AMD64_AUXILIARY_MODULES ${COMPAT_RUNTIME_AUXILIARY_MODULES}
    msvcm80
    msvcm90
    msvcp80
    msvcp90
    msvcr80
    msvcr90)
set(EMU_AMD64_ALIASES ${COMPAT_RUNTIME_ALIASES})

set(EMU_AMD64_EXECUTABLES
    calc
    cmd
    emulation_gui
    emulation_test
    glgears
    glmark2
    glmark2_runner
    hostname
    msiexec
    mspaint
    notepad
    regsvr32
    rundll32
    wglgears_runner
    winver)

set(EMU_AMD64_ARCH_COMMAND_TARGETS)
foreach(_target IN ITEMS
    emulation_gui
    emulation_test
    glgears
    glmark2
    glmark2_runner
    wglgears_runner)
    list(APPEND EMU_AMD64_ARCH_COMMAND_TARGETS "${_target}")
    list(APPEND EMU_AMD64_ALIASES "${_target}=amd64_${_target}${CMAKE_EXECUTABLE_SUFFIX}")
endforeach()
