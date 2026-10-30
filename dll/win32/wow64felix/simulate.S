/*
 * PROJECT:     LiberNT WoW64 x86 CPU
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Enter the recompiler with the native context WoW64 unwinds to
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <asm/ksriscv64.h>

#define SIMULATE_FRAME (ContextLength + 16)

    .text

    .p2align 2
    .globl BTCpuSimulate
    .seh_proc BTCpuSimulate
BTCpuSimulate:
    addi sp, sp, -SIMULATE_FRAME
    .seh_set_cfa x2, SIMULATE_FRAME
    sd ra, (SIMULATE_FRAME - 8)(sp)
    .seh_save_gpr x1, -8
    .seh_endprologue
    mv a0, sp
    call RtlCaptureContext
    mv a0, sp
    call FelixSimulate
    .seh_startepilogue
    ld ra, (SIMULATE_FRAME - 8)(sp)
    .seh_same_gpr x1
    addi sp, sp, SIMULATE_FRAME
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc
