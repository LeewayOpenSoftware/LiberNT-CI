add_library(leewaydisplay_static STATIC
    display_backend.c
    wallpaper_backend.c
    screensaver_backend.c
)

target_include_directories(leewaydisplay_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/include>
)

add_definitions(-DUNICODE -D_UNICODE)
set_target_properties(leewaydisplay_static PROPERTIES FOLDER "Libraries")
add_dependencies(leewaydisplay_static psdk)
