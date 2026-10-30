/*
 * PROJECT:     LiberNT WoW64 layer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     RISC-V64 asm code for syscall.c
 * COPYRIGHT:   Copyright 2021 Alexandre Julliard
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <asm/ksriscv64.h>

#define RAISE_FRAME (ContextLength + 48)

    .text

    .p2align 2
    .globl raise_exception
    .seh_proc raise_exception
raise_exception:
    .seh_handler raise_exception_handler, @except
    addi sp, sp, -RAISE_FRAME
    .seh_set_cfa x2, RAISE_FRAME
    sd ra, 40(sp)
    .seh_save_gpr x1, 40 - RAISE_FRAME
    .seh_endprologue
    sd a0, 0(sp)
    sd a1, 8(sp)
    sd a2, 16(sp)
    sd a3, 24(sp)
    addi a0, sp, 48
    call RtlCaptureContext
    addi a1, sp, 48
    la t0, 1f
    sd t0, CxPc(a1)
    ld a0, 24(sp)
    ld a2, 16(sp)
    call NtRaiseException
    .globl raise_exception_ret
raise_exception_ret:
    ld a0, 0(sp)
    ld a1, 8(sp)
    addi a2, sp, 48
    call call_user_exception_dispatcher
1:  nop
    .seh_startepilogue
    ld ra, 40(sp)
    .seh_same_gpr x1
    addi sp, sp, RAISE_FRAME
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc

    .p2align 2
    .globl raise_exception_handler
    .seh_proc raise_exception_handler
raise_exception_handler:
    addi sp, sp, -16
    .seh_set_cfa x2, 16
    sd ra, 8(sp)
    .seh_save_gpr x1, -8
    .seh_endprologue
    lw t0, 4(a0)
    andi t0, t0, 6
    bnez t0, 1f
    mv a2, a0
    mv a0, a1
    la a1, raise_exception_ret
    li a3, 0
    call RtlUnwind
1:  li a0, 1
    .seh_startepilogue
    ld ra, 8(sp)
    .seh_same_gpr x1
    addi sp, sp, 16
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc

    .p2align 2
    .globl wow64_syscall
    .seh_proc wow64_syscall
wow64_syscall:
    .seh_handler wow64_syscall_handler, @except
    addi sp, sp, -16
    .seh_set_cfa x2, 16
    sd ra, 8(sp)
    .seh_save_gpr x1, -8
    .seh_endprologue
    jalr ra, a1, 0
    j 1f
    .globl wow64_syscall_ret
wow64_syscall_ret:
    li t0, 0xc0000008
    sext.w t0, t0
    bne a0, t0, 1f
    call call_raise_user_exception_dispatcher
1:
    .seh_startepilogue
    ld ra, 8(sp)
    .seh_same_gpr x1
    addi sp, sp, 16
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc

    .p2align 2
    .globl wow64_syscall_handler
    .seh_proc wow64_syscall_handler
wow64_syscall_handler:
    addi sp, sp, -16
    .seh_set_cfa x2, 16
    sd ra, 8(sp)
    .seh_save_gpr x1, -8
    .seh_endprologue
    lw t0, 4(a0)
    andi t0, t0, 6
    bnez t0, 1f
    mv a2, a0
    mv a0, a1
    la a1, wow64_syscall_ret
    lw a3, 0(a2)
    call RtlUnwind
1:  li a0, 1
    .seh_startepilogue
    ld ra, 8(sp)
    .seh_same_gpr x1
    addi sp, sp, 16
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc

    .p2align 2
    .globl cpu_simulate
    .seh_proc cpu_simulate
cpu_simulate:
    .seh_handler cpu_simulate_handler, @except
    addi sp, sp, -16
    .seh_set_cfa x2, 16
    sd ra, 8(sp)
    .seh_save_gpr x1, -8
    .seh_endprologue
    .globl cpu_simulate_loop
cpu_simulate_loop:
    la t0, pBTCpuSimulate
    ld t0, 0(t0)
    jalr ra, t0, 0
    j cpu_simulate_loop
    .seh_endproc

    .p2align 2
    .globl cpu_simulate_handler
    .seh_proc cpu_simulate_handler
cpu_simulate_handler:
    addi sp, sp, -48
    .seh_set_cfa x2, 48
    sd ra, 40(sp)
    .seh_save_gpr x1, -8
    sd s0, 32(sp)
    .seh_save_gpr x8, -16
    sd s1, 24(sp)
    .seh_save_gpr x9, -24
    .seh_endprologue
    mv s0, a0
    mv s1, a1
    lw t0, 4(a0)
    andi t0, t0, 6
    bnez t0, 1f
    sd a0, 0(sp)
    sd a2, 8(sp)
    mv a0, sp
    call Wow64PassExceptionToGuest
    mv a0, s1
    la a1, cpu_simulate_loop
    mv a2, s0
    li a3, 0
    call RtlUnwind
1:  li a0, 1
    .seh_startepilogue
    ld s1, 24(sp)
    .seh_same_gpr x9
    ld s0, 32(sp)
    .seh_same_gpr x8
    ld ra, 40(sp)
    .seh_same_gpr x1
    addi sp, sp, 48
    .seh_set_cfa x2, 0
    ret
    .seh_endepilogue
    .seh_endproc
