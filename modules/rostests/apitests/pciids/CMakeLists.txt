add_executable(pciids_apitest
    pciids.c
    testlist.c)

set_module_type(pciids_apitest win32cui)
add_importlibs(pciids_apitest cfgmgr32 msvcrt kernel32)
add_rostests_file(TARGET pciids_apitest)
