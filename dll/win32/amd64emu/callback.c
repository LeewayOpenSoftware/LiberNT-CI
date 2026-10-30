/*
 * PROJECT:     LiberNT AMD64 emulation host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Deliver kernel user-mode callbacks to the guest the way the AMD64 kernel does
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "amd64emu.h"

enum
{
#define DEFINE_USER32_CALLBACK(id, value, fn) id = value,
#include <u32cb.h>
#undef DEFINE_USER32_CALLBACK
};

static
BOOLEAN
EmuIsImageBase(
    _In_ PVOID Base)
{
    MEMORY_BASIC_INFORMATION Information;

    return NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(),
                                           Base,
                                           MemoryBasicInformation,
                                           &Information,
                                           sizeof(Information),
                                           NULL)) &&
           Information.Type == MEM_IMAGE &&
           Information.AllocationBase == Base;
}

static
VOID
EmuTranslateClientModule(
    _Inout_ PVOID *Module)
{
    MEMORY_BASIC_INFORMATION Information;

    if (EmuIsImageBase(*Module)) return;
    if (NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(),
                                        EmuProcess.GuestPeb->KernelCallbackTable,
                                        MemoryBasicInformation,
                                        &Information,
                                        sizeof(Information),
                                        NULL)) &&
        Information.Type == MEM_IMAGE)
    {
        *Module = Information.AllocationBase;
    }
}

static
NTSTATUS
EmuUserCallback(
    _In_ ULONG Index,
    _In_ PVOID Argument,
    _In_ ULONG Length)
{
    PEMU_THREAD Thread = EmuCurrentThread();
    PAMD64_UCALLOUT_FRAME Frame;
    AMD64_CONTEXT Context;
    EMU_CALLBACK Callback;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Thread || !Thread->Cpu || !Thread->HostDepth) return STATUS_NOT_SUPPORTED;

    Callback.Context.ContextFlags = CONTEXT_AMD64_FULL | CONTEXT_AMD64_SEGMENTS;
    EmuCoreGetContext(Thread->Cpu, &Callback.Context);
    Callback.Frame = EmuCoreGetFrame(Thread->Cpu);
    Callback.HostDepth = Thread->HostDepth;

    Frame = (PVOID)((Callback.Context.Rsp - Length - sizeof(*Frame)) & ~(ULONG64)15);
    _SEH2_TRY
    {
        RtlCopyMemory(Frame + 1, Argument, Length);
        if (Index == USER32_CALLBACK_LOADMENU && Length >= sizeof(PVOID)) EmuTranslateClientModule((PVOID *)(Frame + 1));
        RtlZeroMemory(Frame, sizeof(*Frame));
        Frame->Buffer = (ULONG64)(Frame + 1);
        Frame->Length = Length;
        Frame->ApiNumber = Index;
        Frame->MachineFrame.Rip = Callback.Context.Rip;
        Frame->MachineFrame.SegCs = Callback.Context.SegCs;
        Frame->MachineFrame.EFlags = Callback.Context.EFlags;
        Frame->MachineFrame.Rsp = Callback.Context.Rsp;
        Frame->MachineFrame.SegSs = Callback.Context.SegSs;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status)) return Status;

    Context = Callback.Context;
    Context.ContextFlags = CONTEXT_AMD64_CONTROL;
    Context.Rip = EmuProcess.KiUserCallbackDispatcher;
    Context.Rsp = (ULONG64)Frame;
    EmuCoreSetContext(Thread->Cpu, &Context);

    Callback.Previous = Thread->Callback;
    Thread->Callback = &Callback;
    Thread->HostDepth = 0;
    EmuCoreRun(Thread->Cpu);
}

NTSTATUS
EmuCallbackReturn(
    _In_ PEMU_THREAD Thread,
    _In_opt_ PVOID Result,
    _In_ ULONG ResultLength,
    _In_ NTSTATUS CallbackStatus)
{
    PEMU_CALLBACK Callback = Thread->Callback;
    NTSTATUS Status;

    if (!Callback) return STATUS_NO_CALLBACK_ACTIVE;

    Thread->Callback = Callback->Previous;
    EmuCoreSetContext(Thread->Cpu, &Callback->Context);
    EmuCoreSetFrame(Thread->Cpu, Callback->Frame);
    Thread->HostDepth = Callback->HostDepth;

    Status = NtCallbackReturn(Result, ResultLength, CallbackStatus);
    EmuFatal(Status, "callback return failed");
}

#define DEFINE_USER32_CALLBACK(id, value, fn) \
    static NTSTATUS NTAPI EmuCallback##value(_In_ PVOID Argument, _In_ ULONG Length) \
    { \
        return EmuUserCallback(value, Argument, Length); \
    }
#include <u32cb.h>
#undef DEFINE_USER32_CALLBACK

#define DEFINE_USER32_CALLBACK(id, value, fn) [value] = EmuCallback##value,
static PVOID EmuCallbackTable[] =
{
#include <u32cb.h>
};
#undef DEFINE_USER32_CALLBACK

VOID
EmuInitializeCallbacks(VOID)
{
    NtCurrentPeb()->KernelCallbackTable = EmuCallbackTable;
}
