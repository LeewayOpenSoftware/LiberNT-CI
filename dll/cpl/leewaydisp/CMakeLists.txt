spec2def(leewaydisp.cpl leewaydisp.spec)
file(GLOB leewaydisp_rc_deps res/*.*)
add_rc_deps(leewaydisp.rc ${leewaydisp_rc_deps})

add_library(leewaydisp MODULE
    leewaydisp.c
    leewaydisp.rc
    ${CMAKE_CURRENT_BINARY_DIR}/leewaydisp.def)

set_module_type(leewaydisp cpl UNICODE)
target_link_libraries(leewaydisp leewaydisplay_static)
add_importlibs(leewaydisp user32 gdi32 comctl32 gdiplus advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET leewaydisp DESTINATION reactos/system32 FOR all)
