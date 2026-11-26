
list(APPEND SOURCE
    wlrecovery.c
    wlrecovery.h)

add_library(wlrecovery ${SOURCE})
target_include_directories(wlrecovery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(wlrecovery psdk)
