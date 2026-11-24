
list(APPEND SOURCE
    wlimgcheck.c
    wlimgcheck.h)

add_library(wlimgcheck ${SOURCE})
target_include_directories(wlimgcheck PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(wlimgcheck psdk)
