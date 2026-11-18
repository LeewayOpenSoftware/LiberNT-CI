# The LiberNT default style in the version-4 format, converted from the version-3 style of ../Win8
find_program(LIBERNT_V4_PYTHON NAMES python3 python REQUIRED NO_CMAKE_FIND_ROOT_PATH)

set(_v4_src ${CMAKE_CURRENT_SOURCE_DIR}/../Win8/win8.msstyles)
set(_v4_out ${CMAKE_CURRENT_BINARY_DIR}/v4)
file(GLOB _v4_bitmaps ${_v4_src}/bitmaps/*.bmp)

add_custom_command(
    OUTPUT ${_v4_out}/liberntv4.rc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_v4_out}
    COMMAND ${LIBERNT_V4_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ini2v4.py
        --ini ${_v4_src}/textfiles/NormalNormal.INI
        --bitmaps ${_v4_src}/bitmaps
        --stylemap ${REACTOS_SOURCE_DIR}/dll/win32/uxtheme/stylemap.c
        --headers ${REACTOS_SOURCE_DIR}/sdk/include/psdk/vsstyle.h ${REACTOS_SOURCE_DIR}/sdk/include/psdk/vssym32.h
        --out ${_v4_out} --name liberntv4 --display LiberNT --description "LiberNT v4 default theme"
        --reactos-version
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ini2v4.py ${_v4_src}/textfiles/NormalNormal.INI ${_v4_bitmaps}
            ${REACTOS_SOURCE_DIR}/dll/win32/uxtheme/stylemap.c
    VERBATIM
    COMMENT "Converting the LiberNT style to version 4")

# The directory of the generated blobs is searched for the files the resource script names
include_directories(${_v4_out})
add_library(liberntv4.msstyles MODULE ${_v4_out}/liberntv4.rc)
set_module_type(liberntv4.msstyles module)
add_dependencies(liberntv4.msstyles xdk)
set_target_properties(liberntv4.msstyles PROPERTIES SUFFIX "")
