add_executable(test_perf_consumer test_perf_consumer.c)
set_module_type(test_perf_consumer win32cui)
target_compile_definitions(test_perf_consumer PRIVATE __WINESRC__)
add_importlibs(test_perf_consumer msvcrt kernel32_vista kernel32 advapi32 ntdll)
set_wine_module(test_perf_consumer)

# Experimental Rust kernel-driver PoC (lnrspoc). OFF by default; not on the ISO.
# Enabled only by CI / an explicit -DLIBERNT_ENABLE_RUST=ON configure. See
# tools/rust-poc/README.md.
option(LIBERNT_ENABLE_RUST "Build the experimental Rust kernel-driver PoC (not part of the ISO)" OFF)
if(LIBERNT_ENABLE_RUST)
    add_subdirectory(rust-poc)
endif()
