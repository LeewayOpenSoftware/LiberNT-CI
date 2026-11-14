
PROJECT(NTOS)

if(KDBG AND (ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64") AND NOT TARGET kdb_zydis)
    # KDB must decode the instruction set emitted by current compilers. Build
    # the pinned Zydis copy carried by the vendored ARM64EC runtime source in
    # its allocation-free kernel configuration. Define native ReactOS targets here
    # because the upstream CMake aliases conflict with our clean-target wrapper.
    set(KDB_ZYDIS_ROOT ${REACTOS_SOURCE_DIR}/submodules/fex-arm64ec/External/zydis)
    set(KDB_ZYCORE_ROOT ${KDB_ZYDIS_ROOT}/dependencies/zycore)

    if(NOT EXISTS ${KDB_ZYDIS_ROOT}/src/MetaInfo.c OR NOT EXISTS ${KDB_ZYCORE_ROOT}/src/API/Memory.c)
        message(FATAL_ERROR "KDBG requires Zydis and Zycore from the fex-arm64ec feed. "
            "Run scripts/feeds update fex-arm64ec, or configure with -DKD_DEBUGGER=NONE.")
    endif()

    add_library(kdb_zycore STATIC ${KDB_ZYCORE_ROOT}/src/API/Memory.c ${KDB_ZYCORE_ROOT}/src/API/Process.c ${KDB_ZYCORE_ROOT}/src/API/Synchronization.c ${KDB_ZYCORE_ROOT}/src/API/Terminal.c ${KDB_ZYCORE_ROOT}/src/API/Thread.c ${KDB_ZYCORE_ROOT}/src/Allocator.c ${KDB_ZYCORE_ROOT}/src/ArgParse.c ${KDB_ZYCORE_ROOT}/src/Bitset.c ${KDB_ZYCORE_ROOT}/src/Format.c ${KDB_ZYCORE_ROOT}/src/List.c ${KDB_ZYCORE_ROOT}/src/String.c ${KDB_ZYCORE_ROOT}/src/Vector.c ${KDB_ZYCORE_ROOT}/src/Zycore.c)
    target_compile_definitions(kdb_zycore PUBLIC ZYAN_NO_LIBC ZYCORE_STATIC_BUILD)
    target_include_directories(kdb_zycore PUBLIC ${KDB_ZYCORE_ROOT}/include)

    add_library(kdb_zydis STATIC ${KDB_ZYDIS_ROOT}/src/MetaInfo.c ${KDB_ZYDIS_ROOT}/src/Mnemonic.c ${KDB_ZYDIS_ROOT}/src/Register.c ${KDB_ZYDIS_ROOT}/src/SharedData.c ${KDB_ZYDIS_ROOT}/src/String.c ${KDB_ZYDIS_ROOT}/src/Utils.c ${KDB_ZYDIS_ROOT}/src/Zydis.c ${KDB_ZYDIS_ROOT}/src/Decoder.c ${KDB_ZYDIS_ROOT}/src/DecoderData.c ${KDB_ZYDIS_ROOT}/src/Disassembler.c ${KDB_ZYDIS_ROOT}/src/Formatter.c ${KDB_ZYDIS_ROOT}/src/FormatterBuffer.c ${KDB_ZYDIS_ROOT}/src/FormatterATT.c ${KDB_ZYDIS_ROOT}/src/FormatterBase.c ${KDB_ZYDIS_ROOT}/src/FormatterIntel.c)
    target_compile_definitions(kdb_zydis PUBLIC ZYAN_NO_LIBC ZYCORE_STATIC_BUILD ZYDIS_STATIC_BUILD ZYDIS_DISABLE_ENCODER ZYDIS_DISABLE_SEGMENT)
    target_include_directories(kdb_zydis PUBLIC ${KDB_ZYDIS_ROOT}/include ${KDB_ZYCORE_ROOT}/include PRIVATE ${KDB_ZYDIS_ROOT}/src)
    target_link_libraries(kdb_zydis kdb_zycore)
endif()

if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Enable this again. CORE-17637
    add_compile_options(-Wunused-result)
endif()

include(ntos.cmake)

# LRM-RS: route the special pool through the Rust orchestration and link lrm_rs into ntoskrnl.
# DEFAULT ON on amd64 (owner order 2026-11-14, after validation gates G1-G9); amd64 only (Leeway x64).
# The C special pool stays compiled as oracle/fallback (ntspecial.c #else branch); a flag-OFF build is
# byte-identical to a no-LRM-RS kernel (the self-test export is appended only in a flag-ON build via a
# generated spec). Decoupled from LIBERNT_ENABLE_RUST (the opt-in lnrspoc PoC umbrella, still OFF by
# default): add_rust_staticlib is always available because sdk/cmake/rust.cmake is included uncondition-
# ally. An explicit -DLIBERNT_ENABLE_LRM_RS=ON on a non-amd64 arch stays fatal (see the guard below).
if(ARCH STREQUAL "amd64")
    option(LIBERNT_ENABLE_LRM_RS "Route the special pool through the Rust LRM-RS path" ON)
else()
    option(LIBERNT_ENABLE_LRM_RS "Route the special pool through the Rust LRM-RS path" OFF)
endif()
# LAB ONLY: validate the LRM-RS kernel panic policy (handler -> LEEWAY_RUST_PANIC bug check) in a
# disposable VM. Exposes lrm_rs_force_panic()/LrmRsForcePanic(); since it deliberately panics, this
# build enables the Rust `panic-test` feature AND drops the FORBID_PANIC gate. Never for ship.
option(LIBERNT_LRM_RS_FORCE_PANIC_TEST "LAB ONLY: expose LrmRsForcePanic to test the LRM-RS panic policy in a VM" OFF)

option(NVS "Build the kernel with the NT Virtual Memory Subsystem and cache manager" ON)

if(NOT NVS)
    message(FATAL_ERROR "The legacy memory and cache managers are retired. Configure with NVS=ON.")
endif()

if(NVS)
    file(GLOB NVS_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/core/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/vad/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/fault/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/section/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/ws/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/pagefile/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/vm/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/sys/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/proc/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/nt/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/pool/backing/mmbacking.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/arch/${ARCH}/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/engine/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/api/*.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/backing/*.c)

    list(APPEND SOURCE ${NVS_SOURCE})
    include(nvs/check-standalone.cmake)
    nvs_assert_standalone(${SOURCE} ${ASM_SOURCE})
    add_compile_definitions(NVS)
    include_directories(${REACTOS_SOURCE_DIR}/ntoskrnl/nvs/arch/${ARCH})
endif()

set(NTOSKRNL_SOURCE ${SOURCE})
set(NTOSKRNL_ASM_SOURCE ${ASM_SOURCE})


# Default: the checked-in spec verbatim. Flag-ON (amd64): append the experimental self-test export
# to a generated copy in the binary dir so the DEFAULT export table stays byte-identical. spec2def()
# reads ${CMAKE_CURRENT_SOURCE_DIR}/<arg>, so reference the generated file by a relative path.
set(_NTOSKRNL_SPEC ntoskrnl.spec)
if(LIBERNT_ENABLE_LRM_RS AND ARCH STREQUAL "amd64")
    set(_lrmrs_spec_abs ${CMAKE_CURRENT_BINARY_DIR}/ntoskrnl.lrmrs.spec)
    # Copy the checked-in spec verbatim, then append the one experimental export, so the default
    # build never sees it. configure_file(COPYONLY)+file(APPEND) avoids pulling the spec text through
    # a CMake variable (no ${}/;/\ mangling).
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ntoskrnl.spec ${_lrmrs_spec_abs} COPYONLY)
    file(APPEND ${_lrmrs_spec_abs} "\n@ stdcall -arch=win64 LrmRsSelfTest(ptr)\n")
    if(LIBERNT_LRM_RS_FORCE_PANIC_TEST)
        # Lab-only panic-policy trigger, resolved by lnrspoc via MmGetSystemRoutineAddress.
        file(APPEND ${_lrmrs_spec_abs} "@ stdcall -arch=win64 LrmRsForcePanic(long)\n")
    endif()
    file(RELATIVE_PATH _NTOSKRNL_SPEC ${CMAKE_CURRENT_SOURCE_DIR} ${_lrmrs_spec_abs})
endif()

spec2def(ntoskrnl.exe ${_NTOSKRNL_SPEC} ADD_IMPORTLIB)

# Link GCC stack protector lib
if (STACK_PROTECTOR)
    target_link_libraries(libntoskrnl INTERFACE gcc_ssp_nt)
endif()

if(MSVC_IDE)
    set_source_files_properties(${NTOSKRNL_ASM_SOURCE} PROPERTIES COMPILE_DEFINITIONS "CONFIG_SMP")
endif()

add_asm_files(ntoskrnl_asm ${NTOSKRNL_ASM_SOURCE})

if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64" OR ARCH STREQUAL "riscv64")
    list(APPEND NTOSKRNL_SOURCE ${REACTOS_SOURCE_DIR}/ntoskrnl/ke/${ARCH}/mproc.c)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Clang optimises strcmp calls to memcmp.
    target_link_libraries(libntoskrnl INTERFACE memcmp)
endif()

if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
    if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
        add_compile_options(-mxsave -mxsaves -mxsaveopt)
    endif()
endif()

list(APPEND PCH_SKIP_SOURCE
    guid.c)

# Win11 ABI export parity: spec2def-generated stubs for the unimplemented
# Win11 exports, plus hand-written placeholder data exports. Both are kept out of
# the PCH (ntoskrnl_stubs.c already gets SKIP_PRECOMPILE_HEADERS from spec2def();
# ke/exports.c is header-independent on purpose).
list(APPEND PCH_SKIP_SOURCE
    ke/exports.c
    ${CMAKE_CURRENT_BINARY_DIR}/ntoskrnl_stubs.c)

add_executable(ntoskrnl
    ${ntoskrnl_asm}
    ${NTOSKRNL_SOURCE}
    ${PCH_SKIP_SOURCE}
    ntoskrnl.rc
    ${CMAKE_CURRENT_BINARY_DIR}/ntoskrnl.def)
set_module_type(ntoskrnl kernel)
target_compile_definitions(ntoskrnl PRIVATE CONFIG_SMP)

if(ARCH STREQUAL "arm64")
    target_link_libraries(ntoskrnl rtl_kernel_context)
endif()

source_group(TREE ${REACTOS_SOURCE_DIR}/ntoskrnl PREFIX "Source Files" FILES ${NTOSKRNL_SOURCE})

target_link_libraries(ntoskrnl ntoskrnl_vista cportlib csq ${PSEH_LIB} apisets arblib cmlib ntlsalib rtl cryptlib symz libcntpr setjmp wdmguid poguid ioevent)

if(KDBG AND (ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64"))
    target_link_libraries(ntoskrnl kdb_zydis)
endif()

target_link_libraries(ntoskrnl rtl_vista ci)

if(STACK_PROTECTOR)
    target_link_libraries(ntoskrnl gcc_ssp_nt)
endif()

add_importlibs(ntoskrnl hal kdcom bootvid)
if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" AND ARCH STREQUAL "i386"))
    add_pch(ntoskrnl ${REACTOS_SOURCE_DIR}/ntoskrnl/include/ntoskrnl.h "${PCH_SKIP_SOURCE}")
endif()
add_dependencies(ntoskrnl psdk asm)
add_cd_file(TARGET ntoskrnl DESTINATION reactos/system32 NO_CAB FOR all)

# LRM-RS: build the Rust special-pool crate as a reproducible no_std staticlib and link it into
# ntoskrnl, then define LIBERNT_LRM_RS so ntspecial.c routes through it and lrmrs.c compiles the
# shim + self-test. Opt-in, amd64 only. lrm_rs imports at most one kernel symbol (LrmRsFatalStop, the
# panic-policy target); its runtime helpers resolve from the kernel's own libraries at the ntoskrnl link.
if(LIBERNT_ENABLE_LRM_RS)
    if(NOT ARCH STREQUAL "amd64")
        message(FATAL_ERROR "LIBERNT_ENABLE_LRM_RS is amd64 only (Leeway x64).")
    endif()
    # Decoupled from LIBERNT_ENABLE_RUST (shape B): lrm_rs builds via add_rust_staticlib (always
    # defined by sdk/cmake/rust.cmake), so the production kernel gets the Rust special pool without
    # the opt-in lnrspoc PoC. ENABLE_RUST remains a separate opt-in for tools/rust-poc.
    # lrm_rs imports exactly one kernel symbol: LrmRsFatalStop (the panic-policy target in lrmrs.c).
    # In the normal panic-free build the handler is dead-stripped and even that import vanishes; the
    # allow-list entry only matters if a panic path (or the force-panic test) keeps the handler.
    set(_lrm_rs_features "")
    set(_lrm_rs_forbid FORBID_PANIC)
    if(LIBERNT_LRM_RS_FORCE_PANIC_TEST)
        # Deliberately panics -> enable the feature and DROP FORBID_PANIC (the panic is intentional).
        set(_lrm_rs_features FEATURES panic-test)
        set(_lrm_rs_forbid "")
        target_compile_definitions(ntoskrnl PRIVATE LIBERNT_LRM_RS_FORCE_PANIC_TEST=1)
        message(WARNING "LRM-RS: FORCE-PANIC TEST build - LrmRsForcePanic exposed, FORBID_PANIC disabled (lab only, never ship).")
    endif()
    add_rust_staticlib(lrm_rs
        MANIFEST_DIR ${REACTOS_SOURCE_DIR}/tools/rust-poc/lrm-rs
        CRATE lrm_rs
        ${_lrm_rs_features}
        ALLOWED_EXTERNS LrmRsFatalStop
        ${_lrm_rs_forbid})
    target_link_libraries(ntoskrnl lrm_rs)
    target_compile_definitions(ntoskrnl PRIVATE LIBERNT_LRM_RS=1)
    message(STATUS "LRM-RS: special pool routed through Rust (lrm_rs linked into ntoskrnl)")
endif()

add_subdirectory(nvs)
add_subdirectory(cc)

add_custom_target(nvs_check_standalone
    COMMAND ${CMAKE_COMMAND} -DNVS_KERNEL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/nvs/check-standalone.cmake)
add_dependencies(ntoskrnl nvs_check_standalone)

if(WOW64_I386_RUNTIME AND ARCH STREQUAL "i386")
    set_source_files_properties(ntdll.S ntdll_nt6.S PROPERTIES COMPILE_DEFINITIONS WOW64_I386_RUNTIME)
endif()

add_asm_files(ntdllsys_asm ntdll.S)
add_library(ntdllsys ${ntdllsys_asm})
set_target_properties(ntdllsys PROPERTIES LINKER_LANGUAGE "C")
add_dependencies(ntdllsys asm)

# NT6 syscalls for ntdll_vista
add_asm_files(ntdllsys_nt6_asm ntdll_nt6.S)
add_library(ntdllsys_nt6 ${ntdllsys_nt6_asm})
set_target_properties(ntdllsys_nt6 PROPERTIES LINKER_LANGUAGE "C")
add_dependencies(ntdllsys_nt6 asm)
