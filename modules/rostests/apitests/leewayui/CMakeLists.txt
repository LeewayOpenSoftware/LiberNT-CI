add_executable(leewayui_ribbon_test
    ribbon_test.cpp
    ribbon_test.manifest
)

set_module_type(leewayui_ribbon_test win32gui)

target_link_libraries(leewayui_ribbon_test
    leewayui_static
    kernel32 user32 gdi32
)

add_dependencies(leewayui_ribbon_test psdk)

# Embed manifest
if(MSVC OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_custom_command(TARGET leewayui_ribbon_test POST_BUILD
        COMMAND mt.exe -manifest "${CMAKE_CURRENT_SOURCE_DIR}/ribbon_test.manifest" -outputresource:"$<TARGET_FILE:leewayui_ribbon_test>";1
        VERBATIM
    )
endif()
