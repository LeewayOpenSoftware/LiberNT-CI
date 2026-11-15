
list(APPEND SOURCE
    rtcapi.c
    rtcuserapi.c
)

if(ARCH STREQUAL "i386")
    list(APPEND ASM_SOURCE i386/_RTC_CheckEsp.S)
endif()

add_asm_files(runtmchk_asm ${ASM_SOURCE})
add_library(runtmchk ${SOURCE} ${runtmchk_asm})
add_dependencies(runtmchk asm)

# Flavour for boot-time consumers (FreeLdr, the EFI boot applications): no DbgPrint
# reference, so the linker never pulls DbgPrint (and the firmware entry object behind
# it) out of their own support libraries.
add_asm_files(runtmchk_boot_asm ${ASM_SOURCE})
add_library(runtmchk_boot ${SOURCE} ${runtmchk_boot_asm})
target_compile_definitions(runtmchk_boot PRIVATE RTC_NO_DBGPRINT)
add_dependencies(runtmchk_boot asm)
