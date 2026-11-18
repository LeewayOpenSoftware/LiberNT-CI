
add_subdirectory(interop)
add_subdirectory(newdev)
if(ISAPNP_ENABLE)
    add_subdirectory(isapnp)
endif()
# lntdrv_unittest needs <initializer_list>, which the STLport used by the MSVC build does not have
if(NOT MSVC)
    add_subdirectory(lntdrv)
endif()
add_subdirectory(pathcch)
add_subdirectory(setuplib)
add_subdirectory(terminal)
add_subdirectory(usbaudio)
