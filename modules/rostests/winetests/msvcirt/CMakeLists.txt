add_definitions(-DUSE_WINE_TODOS)

add_executable(msvcirt_winetest msvcirt.c testlist.c)
if(MSVC)
    # C4229: the test declares its function pointers as (*__thiscall name), which MSVC flags as an anachronism
    target_compile_options(msvcirt_winetest PRIVATE /wd4229)
endif()
set_module_type(msvcirt_winetest win32cui)
add_importlibs(msvcirt_winetest msvcrt kernel32)
add_rostests_file(TARGET msvcirt_winetest)
