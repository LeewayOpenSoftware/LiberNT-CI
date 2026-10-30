/*
 * PROJECT:     LiberNT AMD64 emulation host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Deliver user-mode APCs to the guest the way the AMD64 kernel does
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "amd64emu.h"

#define EMU_APC_USER       0x1
#define EMU_APC_PERSISTENT 0x2

typedef struct _EMU_APC
{
    ULONG64 Routine;
    ULONG64 Arguments[3];
    ULONG Flags;
} EMU_APC, *PEMU_APC;

static
VOID
NTAPI
EmuApcThunk(
    _In_ PEMU_APC Apc,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2,
    _In_ PCONTEXT NativeContext)
{
    PEMU_THREAD Thread = EmuCurrentThread();
    PAMD64_APC_FRAME Frame;
    AMD64_CONTEXT Context;
    EMU_APC_RETURN Return;
    ULONG64 Routine = Apc->Routine;
    ULONG64 Arguments[3];
    NTSTATUS Status = STATUS_SUCCESS;

    Arguments[0] = Apc->Arguments[0];
    Arguments[1] = (Apc->Flags & EMU_APC_USER) ? Apc->Arguments[1] : (ULONG64)SystemArgument1;
    Arguments[2] = (Apc->Flags & EMU_APC_USER) ? Apc->Arguments[2] : (ULONG64)SystemArgument2;
    if (!(Apc->Flags & EMU_APC_PERSISTENT)) RtlFreeHeap(RtlGetProcessHeap(), 0, Apc);

    if (!Thread || !Thread->Cpu || !Thread->HostDepth)
    {
        DbgPrint("AMD64EMU: dropping an APC to %I64x outside a guest system call\n", Routine);
        return;
    }

    Context.ContextFlags = CONTEXT_AMD64_FULL | CONTEXT_AMD64_SEGMENTS;
    EmuCoreGetContext(Thread->Cpu, &Context);
    Frame = (PVOID)((Context.Rsp - sizeof(*Frame)) & ~(ULONG64)15);
    _SEH2_TRY
    {
        RtlZeroMemory(Frame, sizeof(*Frame));
        Frame->Context = Context;
        Frame->Context.ContextFlags = CONTEXT_AMD64_FULL | CONTEXT_AMD64_SEGMENTS;
        Frame->Context.P1Home = Arguments[0];
        Frame->Context.P2Home = Arguments[1];
        Frame->Context.P3Home = Arguments[2];
        Frame->Context.P4Home = Routine;
        Frame->MachineFrame.Rip = Context.Rip;
        Frame->MachineFrame.SegCs = Context.SegCs;
        Frame->MachineFrame.EFlags = Context.EFlags;
        Frame->MachineFrame.Rsp = Context.Rsp;
        Frame->MachineFrame.SegSs = Context.SegSs;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("AMD64EMU: cannot build the APC frame for %I64x: %08lx\n", Routine, Status);
        return;
    }

    Context.ContextFlags = CONTEXT_AMD64_CONTROL;
    Context.Rip = EmuProcess.KiUserApcDispatcher;
    Context.Rsp = (ULONG64)Frame;
    EmuCoreSetContext(Thread->Cpu, &Context);

    Return.Previous = Thread->ApcReturn;
    Return.Frame = Frame;
    Return.NativeContext = NativeContext;
    Return.CoreFrame = EmuCoreGetFrame(Thread->Cpu);
    Return.HostDepth = Thread->HostDepth;
    Thread->ApcReturn = &Return;
    Thread->HostDepth = 0;
    EmuCoreRun(Thread->Cpu);
}

static
BOOLEAN
EmuIsLocalThread(
    _In_ HANDLE ThreadHandle)
{
    THREAD_BASIC_INFORMATION Information;

    if (ThreadHandle == NtCurrentThread()) return TRUE;
    if (!NT_SUCCESS(NtQueryInformationThread(ThreadHandle, ThreadBasicInformation, &Information,
                                             sizeof(Information), NULL)))
    {
        return FALSE;
    }
    return Information.ClientId.UniqueProcess == NtCurrentTeb()->ClientId.UniqueProcess;
}

BOOLEAN
EmuIsApcReturn(
    _In_ PEMU_THREAD Thread,
    _In_ const AMD64_CONTEXT *Context)
{
    return Thread->ApcReturn && Context == &Thread->ApcReturn->Frame->Context;
}

NTSTATUS
EmuApcReturn(
    _In_ PEMU_THREAD Thread,
    _In_ const AMD64_CONTEXT *Context)
{
    PEMU_APC_RETURN Return = Thread->ApcReturn;
    AMD64_CONTEXT Captured = *Context;
    NTSTATUS Status;

    if ((Captured.ContextFlags & CONTEXT_AMD64) != CONTEXT_AMD64) return STATUS_INVALID_PARAMETER;

    Thread->ApcReturn = Return->Previous;
    EmuCoreSetContext(Thread->Cpu, &Captured);
    EmuCoreSetFrame(Thread->Cpu, Return->CoreFrame);
    Thread->HostDepth = Return->HostDepth;

    Status = NtContinue(Return->NativeContext, TRUE);
    EmuFatal(Status, "APC return failed");
}

static
PEMU_APC
EmuAllocateApc(
    _In_ ULONG64 Routine,
    _In_ ULONG64 Argument1,
    _In_ ULONG64 Argument2,
    _In_ ULONG64 Argument3,
    _In_ ULONG Flags)
{
    PEMU_APC Apc = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Apc));

    if (!Apc) return NULL;
    Apc->Routine = Routine;
    Apc->Arguments[0] = Argument1;
    Apc->Arguments[1] = Argument2;
    Apc->Arguments[2] = Argument3;
    Apc->Flags = Flags;
    return Apc;
}

NTSTATUS
EmuQueueUserApc(
    _In_ ULONG Service,
    _Inout_ PULONG_PTR Arguments,
    _In_ ULONG RoutineIndex)
{
    PEMU_APC Apc;
    NTSTATUS Status;

    if (!EmuIsLocalThread((HANDLE)Arguments[0])) return STATUS_NOT_IMPLEMENTED;

    Apc = EmuAllocateApc(Arguments[RoutineIndex],
                         Arguments[RoutineIndex + 1],
                         Arguments[RoutineIndex + 2],
                         Arguments[RoutineIndex + 3],
                         EMU_APC_USER);
    if (!Apc) return STATUS_NO_MEMORY;

    Arguments[RoutineIndex] = (ULONG_PTR)EmuApcThunk;
    Arguments[RoutineIndex + 1] = (ULONG_PTR)Apc;
    Arguments[RoutineIndex + 2] = 0;
    Arguments[RoutineIndex + 3] = 0;
    Status = (NTSTATUS)EmuInvokeSystemService(Service, Arguments);
    if (!NT_SUCCESS(Status)) RtlFreeHeap(RtlGetProcessHeap(), 0, Apc);
    return Status;
}

BOOLEAN
EmuWrapCompletionApc(
    _Inout_ PULONG_PTR Arguments,
    _In_ ULONG RoutineIndex,
    _In_ BOOLEAN Persistent,
    _Out_ PVOID *Record)
{
    PEMU_APC Apc;

    *Record = NULL;
    if (!Arguments[RoutineIndex]) return TRUE;

    Apc = EmuAllocateApc(Arguments[RoutineIndex], Arguments[RoutineIndex + 1], 0, 0,
                         Persistent ? EMU_APC_PERSISTENT : 0);
    if (!Apc) return FALSE;

    Arguments[RoutineIndex] = (ULONG_PTR)EmuApcThunk;
    Arguments[RoutineIndex + 1] = (ULONG_PTR)Apc;
    *Record = Apc;
    return TRUE;
}

VOID
EmuCompleteApcWrap(
    _In_opt_ PVOID Record,
    _In_ NTSTATUS Status)
{
    if (Record && NT_ERROR(Status)) RtlFreeHeap(RtlGetProcessHeap(), 0, Record);
}
