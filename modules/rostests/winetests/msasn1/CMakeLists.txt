
add_definitions(-DUSE_WINE_TODOS)

list(APPEND SOURCE
    asn1.c
    testlist.c)

add_executable(msasn1_winetest ${SOURCE})
set_module_type(msasn1_winetest win32cui)
add_importlibs(msasn1_winetest msasn1 msvcrt kernel32)
add_rostests_file(TARGET msasn1_winetest)
