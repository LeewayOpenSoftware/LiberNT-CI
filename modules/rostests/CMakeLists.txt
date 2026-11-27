
start_module_group(rostests)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_link_options("-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/test-section.lds")
endif()

add_definitions(-DHAVE_APITEST)

if(MSVC_IDE)
    add_definitions(-DWINETEST_MSVC_IDE_FORMATTING)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options("-Wno-misleading-indentation") # Cause by todo_wine, etc.
endif()

# These rostests targets do not yet build with MSVC and are excluded from the
# MSVC build only (GCC still builds them). Owner of the fixes: Phyllis. Remove
# each name from this list as its MSVC build is fixed; this is an explicit list,
# not a global skip.
set(ROSTESTS_MSVC_EXCLUDED "")
if(MSVC)
    list(APPEND ROSTESTS_MSVC_EXCLUDED acpi rosautotest lntdrv uxtheme)
endif()

add_subdirectory(apitests)
add_subdirectory(aerowizard)
#add_subdirectory(dibtests)
add_subdirectory(drivers)
#add_subdirectory(dxtest)
add_subdirectory(kmtests)
#add_subdirectory(regtests)
if(NOT "rosautotest" IN_LIST ROSTESTS_MSVC_EXCLUDED)
    add_subdirectory(rosautotest)
endif()
add_subdirectory(tests)
add_subdirectory(unittests)
add_subdirectory(win32)
add_subdirectory(winetestlib)
add_subdirectory(winetests)


## Extra files needed for the various tests can be added into the "testdata" subdirectory.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/testdata/)
    file(GLOB_RECURSE TESTDATA_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/ ${CMAKE_CURRENT_SOURCE_DIR}/testdata/*)
    foreach(item ${TESTDATA_FILES})
        add_rostests_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/testdata/${item} SUBDIR testdata)
    endforeach(item)
endif()

end_module_group()
