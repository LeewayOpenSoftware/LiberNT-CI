# Theme Loader 4 (Leeway Open Software), vendored at the commit named in TL4_COMMIT. Do not edit these files here:
# a change is made in the TL4 repository and imported as a whole; MANIFEST.sha256 lists every vendored file.
# I1a-T uses the trust decisions (trust.c) and the bounded PE reader (pe.c); the version-4 loader comes with I1a-V.

add_library(tl4 STATIC
    src/pe.c
    src/trust.c)
target_include_directories(tl4 PUBLIC include)
add_dependencies(tl4 psdk)
