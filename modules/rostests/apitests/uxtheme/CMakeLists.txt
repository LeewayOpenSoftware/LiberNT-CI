
list(APPEND SOURCE
    CaptionGlyphs.c
    CloseThemeData.c
    ColorPreference.c
    DrawThemeParentBackground.c
    GetThemeParseErrorInfo.c
    MenuTheme.c
    NonClientBorder.c
    NonClientCaption.c
    NonClientHitTest.c
    SetWindowTheme.c
    TextScaling.c
    ThemeDpi.c
    SetThemeAppProperties.c
    ThemeRendering.c
    ThemeShellTransition.c
    ThemeSysSize.c
    UxIni.c
    ../../../../dll/win32/uxtheme/uxini.c
    ../include/msgtrace.c
    testlist.c)

add_executable(uxtheme_apitest ${SOURCE})
target_link_libraries(uxtheme_apitest wine ${PSEH_LIB})
set_module_type(uxtheme_apitest win32cui)
add_importlibs(uxtheme_apitest uxtheme comctl32 user32 gdi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET uxtheme_apitest)
