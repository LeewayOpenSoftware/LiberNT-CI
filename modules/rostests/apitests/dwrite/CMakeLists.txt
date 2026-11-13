
add_executable(dwrite_apitest BidiLevels.c FontFallbackLocale.c FontFallbackSource.c GlyphJustification.c HasVariations.c SegoeUiMetrics.c SystemFontFallback.c TrailingWhitespace.c testlist.c)
set_module_type(dwrite_apitest win32cui)
add_importlibs(dwrite_apitest dwrite gdi32 user32 msvcrt kernel32)
add_rostests_file(TARGET dwrite_apitest)
