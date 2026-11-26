/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS Run-Time Library
 * PURPOSE:           AMD64 stubs
 * FILE:              lib/rtl/amd64/stubs.c
 * PROGRAMMERS:        Stefan Ginsberg (stefan.ginsberg@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#define NDEBUG
#include <debug.h>
#include "amd64/ketypes.h"

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
VOID
NTAPI
RtlInitializeContext(
    _Reserved_ HANDLE ProcessHandle,
    _Out_ PCONTEXT ThreadContext,
    _In_opt_ PVOID ThreadStartParam,
    _In_ PTHREAD_START_ROUTINE ThreadStartAddress,
    _In_ PINITIAL_TEB InitialStack)
{
    RtlZeroMemory(ThreadContext, sizeof(*ThreadContext));

    /*
     * Copy what the caller passed, as Windows does: the caller chooses the
     * first instruction (usually RtlUserThreadStart) and the stack pointer.
     * The selectors stay 0; the kernel loads the user ones and reserves the
     * home area when it creates the thread.
     */
    ThreadContext->Rip = (ULONG64)ThreadStartAddress;
    ThreadContext->Rsp = (ULONG64)InitialStack;
    ThreadContext->Rcx = (ULONG64)ThreadStartParam;
    ThreadContext->EFlags = EFLAGS_INTERRUPT_MASK;
    ThreadContext->MxCsr = INITIAL_MXCSR;

    ThreadContext->ContextFlags = CONTEXT_CONTROL |
                                  CONTEXT_INTEGER |
                                  CONTEXT_FLOATING_POINT;
}

NTSTATUS
NTAPI
RtlQueueApcWow64Thread(
    _In_ HANDLE ThreadHandle,
    _In_ PKNORMAL_ROUTINE ApcRoutine,
    _In_opt_ PVOID NormalContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

