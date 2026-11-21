
list(APPEND SOURCE
    binhive.c
    cmi.c
    mkhive.c
    reginf.c
    registry.c
    rtl.c)

add_host_tool(mkhive ${SOURCE})
target_include_directories(mkhive PRIVATE ${REACTOS_SOURCE_DIR}/sdk/lib/rtl)
target_compile_definitions(mkhive PRIVATE MKHIVE_HOST)
if(NOT MSVC)
    target_compile_options(mkhive PRIVATE "-fshort-wchar")
endif()

target_link_libraries(mkhive PRIVATE host_includes unicode cmlibhost inflibhost)

# mkhive stamps every registry key LastWriteTime with the time its
# KeQuerySystemTime() stub reports, so it must never be the wall clock:
# the built hives ship in the image and must stay byte-reproducible.
# Precedence: SOURCE_DATE_EPOCH env (reproducible-builds standard), then
# the source commit date recorded by CMake at configure time, then a
# fixed date. rtl.c falls back to a compile-time default if this is unset.
if(DEFINED ENV{SOURCE_DATE_EPOCH} AND "$ENV{SOURCE_DATE_EPOCH}" MATCHES "^[1-9][0-9]*$")
    set(MKHIVE_BUILD_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
else()
    execute_process(
        COMMAND git log -1 --format=%ct
        WORKING_DIRECTORY "${REACTOS_SOURCE_DIR}"
        OUTPUT_VARIABLE MKHIVE_GIT_EPOCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(MKHIVE_GIT_EPOCH MATCHES "^[1-9][0-9]*$")
        set(MKHIVE_BUILD_EPOCH "${MKHIVE_GIT_EPOCH}")
    else()
        set(MKHIVE_BUILD_EPOCH "1767225600") # 2026-01-01T00:00:00Z
    endif()
endif()
target_compile_definitions(mkhive PRIVATE "MKHIVE_BUILD_EPOCH=${MKHIVE_BUILD_EPOCH}ULL")
