# Theme Loader 4 (Leeway Open Software), vendored at the commit named in TL4_COMMIT. Do not edit these files here:
# a change is made in the TL4 repository and imported as a whole; MANIFEST.sha256 lists every vendored file.
# I1a-T uses the trust decisions (trust.c) and the bounded PE reader (pe.c); the version-4 loader comes with I1a-V.

add_library(tl4 STATIC
    src/pe.c
    src/trust.c)
target_include_directories(tl4 PUBLIC include)
add_dependencies(tl4 psdk)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/MANIFEST.sha256 _tl4_manifest)
foreach(_tl4_line ${_tl4_manifest})
    if(NOT _tl4_line MATCHES "^([0-9a-f]+) [*](.+)$")
        message(FATAL_ERROR "sdk/lib/tl4: malformed MANIFEST.sha256 line: ${_tl4_line}")
    endif()
    set(_tl4_want ${CMAKE_MATCH_1})
    set(_tl4_file ${CMAKE_MATCH_2})
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/${_tl4_file} _tl4_text)
    string(REPLACE "\r\n" "\n" _tl4_text "${_tl4_text}")
    string(SHA256 _tl4_have "${_tl4_text}")
    if(NOT _tl4_have STREQUAL _tl4_want)
        message(FATAL_ERROR "sdk/lib/tl4: ${_tl4_file} differs from the vendored ThemeLoader4 commit (MANIFEST.sha256)")
    endif()
endforeach()
