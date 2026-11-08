add_executable(leewayui_ribbon_test
    ribbon_test.cpp
    ribbon_test.rc
)

set_module_type(leewayui_ribbon_test win32gui)

target_link_libraries(leewayui_ribbon_test
    leewayui_static
    kernel32 user32 gdi32
)

add_dependencies(leewayui_ribbon_test psdk)
