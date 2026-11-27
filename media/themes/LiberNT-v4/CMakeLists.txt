# The LiberNT default style in the version-4 format, converted from the version-3 style of ../Win8.
# liberntv4: the Aero look (composited Liquid materials, used while the compositor runs).
# liberntbasicv4: the same style with every Liquid material off (plain frames and shell, no compositor needed).
find_program(LIBERNT_V4_PYTHON NAMES python3 python REQUIRED NO_CMAKE_FIND_ROOT_PATH)

set(_v4_src ${CMAKE_CURRENT_SOURCE_DIR}/../Win8/win8.msstyles)
file(GLOB _v4_bitmaps ${_v4_src}/bitmaps/*.bmp)

# Installed into the trusted store as %SystemRoot%\Resources\Themes\<store>\<store>.msstyles, the names uxtheme
# chooses by default (OrosiAero, or Orosi when an administrator disabled Mica).
function(add_liberntv4_style _name _store _display _description)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/${_name})
    add_custom_command(
        OUTPUT ${_out}/${_name}.rc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_out}
        COMMAND ${LIBERNT_V4_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ini2v4.py
            --ini ${_v4_src}/textfiles/NormalNormal.INI
            --bitmaps ${_v4_src}/bitmaps
            --stylemap ${REACTOS_SOURCE_DIR}/dll/win32/uxtheme/stylemap.c
            --headers ${REACTOS_SOURCE_DIR}/sdk/include/psdk/vsstyle.h ${REACTOS_SOURCE_DIR}/sdk/include/psdk/vssym32.h
            --out ${_out} --name ${_name} --display ${_display} --description ${_description}
            --reactos-version ${ARGN}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ini2v4.py ${_v4_src}/textfiles/NormalNormal.INI ${_v4_bitmaps}
                ${REACTOS_SOURCE_DIR}/dll/win32/uxtheme/stylemap.c
        VERBATIM
        COMMENT "Converting the LiberNT style to version 4 (${_name})")
    add_library(${_name}.msstyles MODULE ${_out}/${_name}.rc)
    target_include_directories(${_name}.msstyles PRIVATE ${_out})
    set_module_type(${_name}.msstyles module)
    add_dependencies(${_name}.msstyles xdk)
    set_target_properties(${_name}.msstyles PROPERTIES SUFFIX "")
    add_cd_file(TARGET ${_name}.msstyles DESTINATION reactos/Resources/Themes/${_store} NAME_ON_CD ${_store}.msstyles FOR all)
endfunction()

add_liberntv4_style(liberntv4 OrosiAero Orosi "Orosi default theme")
add_liberntv4_style(liberntbasicv4 Orosi "Orosi Basic" "Orosi Basic theme" --basic)
