/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Speculation control stubs called from patched entry and exit sites
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <asm.inc>
#include <ksamd64.inc>

EXTERN KiSpecCtrlKernelCpu:QWORD
EXTERN KiSpecCtrlUserCpu:QWORD

.code64

/* Every stub preserves all registers and flags: the sites sit between arbitrary instructions */

PUBLIC KiSpecCtrlEnterKernel
KiSpecCtrlEnterKernel:
    push rax
    push rcx
    push rdx
    mov rcx, gs:[PcCurrentPrcb]
    movzx ecx, byte ptr [rcx + PbNumber]
    lea rax, KiSpecCtrlKernelCpu[rip]
    mov eax, dword ptr [rax + rcx * 8]
    mov ecx, HEX(48)
    mov edx, 0
    wrmsr
    pop rdx
    pop rcx
    pop rax
    ret

PUBLIC KiSpecCtrlExitKernel
KiSpecCtrlExitKernel:
    push rax
    push rcx
    push rdx
    mov rcx, gs:[PcCurrentPrcb]
    movzx ecx, byte ptr [rcx + PbNumber]
    lea rax, KiSpecCtrlUserCpu[rip]
    mov eax, dword ptr [rax + rcx * 8]
    mov ecx, HEX(48)
    mov edx, 0
    wrmsr
    pop rdx
    pop rcx
    pop rax
    ret

/* Overwrite all 32 return stack buffer entries with harmless targets */
PUBLIC KiSpecFillReturnStack
KiSpecFillReturnStack:
    pushfq
    push rcx
    mov ecx, 16
KiSpecFillLoop:
    call KiSpecFillFirst
KiSpecFillTrap1:
    pause
    lfence
    jmp KiSpecFillTrap1
KiSpecFillFirst:
    call KiSpecFillSecond
KiSpecFillTrap2:
    pause
    lfence
    jmp KiSpecFillTrap2
KiSpecFillSecond:
    dec ecx
    jnz KiSpecFillLoop
    add rsp, 32 * 8
    pop rcx
    popfq
    ret

END
