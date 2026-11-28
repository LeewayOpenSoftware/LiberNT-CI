
spec2def(credprov_testmod.dll testmod.spec)
add_library(credprov_testmod MODULE testmod.c ${CMAKE_CURRENT_BINARY_DIR}/credprov_testmod.def)
set_module_type(credprov_testmod win32dll)
target_link_libraries(credprov_testmod uuid)
add_importlibs(credprov_testmod ole32 msvcrt kernel32)
add_rostests_file(TARGET credprov_testmod)

spec2def(credprov_absent.dll absent.spec ADD_IMPORTLIB)
add_library(credprov_absent MODULE absent.c ${CMAKE_CURRENT_BINARY_DIR}/credprov_absent.def)
set_module_type(credprov_absent win32dll ENTRYPOINT 0)
add_importlibs(credprov_absent kernel32)

spec2def(credprov_testdep.dll testdep.spec)
add_library(credprov_testdep MODULE testmod.c testdep.c ${CMAKE_CURRENT_BINARY_DIR}/credprov_testdep.def)
set_module_type(credprov_testdep win32dll)
target_link_libraries(credprov_testdep uuid)
add_importlibs(credprov_testdep credprov_absent ole32 msvcrt kernel32)
add_rostests_file(TARGET credprov_testdep)
