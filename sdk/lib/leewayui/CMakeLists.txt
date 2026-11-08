add_library(leewayui_static STATIC
    ribbon.cpp
    theme.cpp
    dpi.cpp
)

target_include_directories(leewayui_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/include>
)

set_target_properties(leewayui_static PROPERTIES
    CXX_STANDARD 14
    FOLDER "Libraries"
)
