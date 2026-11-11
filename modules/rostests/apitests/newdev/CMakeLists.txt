add_executable(newdev_apitest
    DiRollbackDriver.c
    protocol.c
    UpdateDriver.c
    testlist.c)

set_module_type(newdev_apitest win32cui)
add_importlibs(newdev_apitest newdev setupapi user32 msvcrt kernel32)
target_link_libraries(newdev_apitest uuid)
add_rostests_file(TARGET newdev_apitest)
