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

.code

/* Every stub preserves all registers and flags: the sites sit between arbitrary instructions.
   Only the paranoid entry changes flags, so only it saves them.
   Unwind info lets a stack walk from an NMI or a debugger break-in get through them. */

MACRO(SPEC_STUB_PROLOG)
    push rax
    .pushreg rax
    push rcx
    .pushreg rcx
    push rdx
    .pushreg rdx
    .endprolog
ENDM

MACRO(SPEC_STUB_EPILOG)
    pop rdx
    pop rcx
    pop rax
    ret
ENDM

PUBLIC KiSpecCtrlEnterKernel
FUNC KiSpecCtrlEnterKernel
    SPEC_STUB_PROLOG
    mov rcx, gs:[PcCurrentPrcb]
    movzx ecx, byte ptr [rcx + PbNumber]
    lea rax, KiSpecCtrlKernelCpu[rip]
    mov eax, dword ptr [rax + rcx * 8]
    mov ecx, HEX(48)
    mov edx, 0
    wrmsr
    SPEC_STUB_EPILOG
ENDFUNC

PUBLIC KiSpecCtrlExitKernel
FUNC KiSpecCtrlExitKernel
    SPEC_STUB_PROLOG
    mov rcx, gs:[PcCurrentPrcb]
    movzx ecx, byte ptr [rcx + PbNumber]
    lea rax, KiSpecCtrlUserCpu[rip]
    mov eax, dword ptr [rax + rcx * 8]
    mov ecx, HEX(48)
    mov edx, 0
    wrmsr
    SPEC_STUB_EPILOG
ENDFUNC

/*
 * NMI, #MC and #DF from kernel mode: rbp is the trap frame, but gs may still be the user one
 * (the exit path between the exit stub and iretq, or a system call before swapgs).
 * Keep what SPEC_CTRL held and add IBRS; the per-processor kernel value is exactly that.
 */
PUBLIC KiSpecCtrlEnterParanoid
FUNC KiSpecCtrlEnterParanoid
    pushfq
    .allocstack 8
    SPEC_STUB_PROLOG
    mov ecx, HEX(48)
    rdmsr
    mov dword ptr [rbp + KTRAP_FRAME_LastExceptionFromRip], eax
    mov dword ptr [rbp + KTRAP_FRAME_LastExceptionFromRip + 4], edx
    or eax, 1
    wrmsr
    pop rdx
    pop rcx
    pop rax
    popfq
    ret
ENDFUNC

/* Before iretq to kernel mode: give back what the interrupted code ran with */
PUBLIC KiSpecCtrlExitParanoid
FUNC KiSpecCtrlExitParanoid
    SPEC_STUB_PROLOG
    mov eax, dword ptr [rbp + KTRAP_FRAME_LastExceptionFromRip]
    mov edx, dword ptr [rbp + KTRAP_FRAME_LastExceptionFromRip + 4]
    mov ecx, HEX(48)
    wrmsr
    SPEC_STUB_EPILOG
ENDFUNC

/* Overwrite all 32 return stack buffer entries with harmless targets; the 32 pushed
   return addresses are dropped before the epilog, so only the prolog needs unwind codes */
PUBLIC KiSpecFillReturnStack
FUNC KiSpecFillReturnStack
    pushfq
    .allocstack 8
    push rcx
    .pushreg rcx
    .endprolog
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
ENDFUNC

END
