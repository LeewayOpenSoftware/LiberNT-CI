project(MSPAINT)

add_definitions(-DINITGUID)

if(DBG)
    add_definitions(-D_DEBUG=1) # CORE-17505
endif()

list(APPEND SOURCE
    canvas.cpp
    dialogs.cpp
    dib.cpp
    drawing.cpp
    fullscreen.cpp
    history.cpp
    main.cpp
    miniature.cpp
    mouse.cpp
    paint_ribbon.cpp
    paint_print.cpp
    palette.cpp
    palettemodel.cpp
    registry.cpp
    selectionmodel.cpp
    sizebox.cpp
    textedit.cpp
    toolbox.cpp
    toolsettings.cpp
    toolsmodel.cpp
    precomp.h)

file(GLOB mspaint_rc_deps img/*.*)
add_rc_deps(rsrc.rc ${mspaint_rc_deps})
add_executable(mspaint ${SOURCE} rsrc.rc)
set_module_type(mspaint win32gui UNICODE)
target_link_libraries(mspaint uuid cpprt atl_classes leewayui_static)
set_target_cpp_properties(mspaint WITH_EXCEPTIONS)
add_importlibs(mspaint comdlg32 shell32 user32 gdi32 advapi32 comctl32 uxtheme msvcrt kernel32 rpcrt4 shlwapi ntdll)
add_pch(mspaint precomp.h SOURCE)
add_cd_file(TARGET mspaint DESTINATION reactos/system32 FOR all)

# Aureline ribbon icons are loaded from <exe dir>\icons\<set>\ at run time (aureline_icons.cpp)
file(GLOB _mspaint_icons_common ${CMAKE_SOURCE_DIR}/sdk/lib/leewayui/icons/common/*.ico)
file(GLOB _mspaint_icons_paint ${CMAKE_SOURCE_DIR}/sdk/lib/leewayui/icons/paint/*.ico)
add_cd_file(FILE ${_mspaint_icons_common} DESTINATION reactos/system32/icons/common FOR all)
add_cd_file(FILE ${_mspaint_icons_paint} DESTINATION reactos/system32/icons/paint FOR all)
