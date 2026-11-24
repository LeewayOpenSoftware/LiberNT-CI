
list(APPEND SOURCE
    wluiauth.c
    wluistate.c
    wluiauth.h)

add_library(wluiauth ${SOURCE})
target_include_directories(wluiauth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(wluiauth psdk)
