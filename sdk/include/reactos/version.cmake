option(NO_REACTOS_BUILDNO "If true, disables the generation of buildno.h and version.h for each configure" OFF)

if (NOT NO_REACTOS_BUILDNO)
    string(TIMESTAMP KERNEL_VERSION_BUILD %Y%m%d UTC)
else()
    set(KERNEL_VERSION_BUILD "custom")
endif()

set(KERNEL_VERSION_MAJOR "0")
set(KERNEL_VERSION_MINOR "4")
set(KERNEL_VERSION_PATCH_LEVEL "17")
set(COPYRIGHT_YEAR "2026")

# KERNEL_VERSION_BUILD_TYPE is "dev" for Git builds
# or "RC1", "RC2", "" for releases.
set(KERNEL_VERSION_BUILD_TYPE "dev")

set(KERNEL_VERSION "${KERNEL_VERSION_MAJOR}.${KERNEL_VERSION_MINOR}.${KERNEL_VERSION_PATCH_LEVEL}-${WINARCH}")
if(NOT KERNEL_VERSION_BUILD_TYPE STREQUAL "")
    set(KERNEL_VERSION "${KERNEL_VERSION}-${KERNEL_VERSION_BUILD_TYPE}")
endif()

math(EXPR REACTOS_DLL_VERSION_MAJOR "${KERNEL_VERSION_MAJOR}+42")
set(DLL_VERSION_STR "${REACTOS_DLL_VERSION_MAJOR}.${KERNEL_VERSION_MINOR}.${KERNEL_VERSION_PATCH_LEVEL}")
if(NOT KERNEL_VERSION_BUILD_TYPE STREQUAL "")
    set(DLL_VERSION_STR "${DLL_VERSION_STR}-${KERNEL_VERSION_BUILD_TYPE}")
endif()

# Leeway cut number, shown in the About boxes as "LBI <number>" (1060, 1060.10, ...).
# One source of truth: -DLIBERNT_LBI_BUILD=<number> wins; otherwise the one-line file
# LBI_BUILD at the tree root (each cut branch carries its own); otherwise "dev".
# The LBI ledger sequence (sdk/cmake/lbi.cmake) stays the per-attempt identity and is
# not derived from this number; the NT identity (VER_PRODUCTBUILD) is untouched.
set(LIBERNT_LBI_BUILD "" CACHE STRING "Leeway cut number shown as 'LBI <number>' in About boxes (empty: LBI_BUILD file, else dev)")
set(LIBERNT_LBI_BUILD_EFFECTIVE "${LIBERNT_LBI_BUILD}")
if(LIBERNT_LBI_BUILD_EFFECTIVE STREQUAL "" AND EXISTS "${REACTOS_SOURCE_DIR}/LBI_BUILD")
    file(STRINGS "${REACTOS_SOURCE_DIR}/LBI_BUILD" _lbi_build_lines LIMIT_COUNT 1)
    if(_lbi_build_lines)
        list(GET _lbi_build_lines 0 LIBERNT_LBI_BUILD_EFFECTIVE)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REACTOS_SOURCE_DIR}/LBI_BUILD")
endif()
string(STRIP "${LIBERNT_LBI_BUILD_EFFECTIVE}" LIBERNT_LBI_BUILD_EFFECTIVE)
if(LIBERNT_LBI_BUILD_EFFECTIVE STREQUAL "")
    set(LIBERNT_LBI_BUILD_EFFECTIVE "dev")
endif()
if(NOT LIBERNT_LBI_BUILD_EFFECTIVE MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    message(FATAL_ERROR "LIBERNT_LBI_BUILD '${LIBERNT_LBI_BUILD_EFFECTIVE}' must be letters, digits, '.', '_' or '-' (e.g. 1060.10)")
endif()
message(STATUS "LBI build number: ${LIBERNT_LBI_BUILD_EFFECTIVE}")

if(NOT LIBERNT_BUILD_NUMBER)
    set(LIBERNT_BUILD_NUMBER "$ENV{GITHUB_RUN_NUMBER}")
endif()
if(NOT LIBERNT_BUILD_NUMBER MATCHES "^[0-9]+$")
    set(LIBERNT_BUILD_NUMBER 0)
endif()

# Get Git revision through "git describe"
set(COMMIT_HASH "unknown-hash")
set(REVISION "unknown-revision")

if((EXISTS "${REACTOS_SOURCE_DIR}/.git") AND (NOT NO_REACTOS_BUILDNO))
    find_package(Git)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(COMMIT_HASH "${GIT_COMMIT_HASH}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --abbrev=7 --long --always
            WORKING_DIRECTORY ${REACTOS_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_DESCRIBE_REVISION
            RESULT_VARIABLE GIT_CALL_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(GIT_CALL_RESULT EQUAL 0)
            set(REVISION "${GIT_DESCRIBE_REVISION}")
        endif()
    endif()
endif()

configure_file(sdk/include/reactos/version.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/version.h)
configure_file(sdk/include/reactos/buildno.h.cmake ${REACTOS_BINARY_DIR}/sdk/include/reactos/buildno.h)
