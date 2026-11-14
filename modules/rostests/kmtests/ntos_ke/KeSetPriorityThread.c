/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     KeSetPriorityThread sets the run-time priority it is asked for and reports the old one
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

typedef struct _SET_PRIORITY_CONTEXT
{
    KEVENT Done;
    KPRIORITY Base;
    KPRIORITY Requested[8];
    KPRIORITY Seen[8];
    KPRIORITY Returned[8];
    ULONG Count;
} SET_PRIORITY_CONTEXT, *PSET_PRIORITY_CONTEXT;

/* Runs in a fresh system thread: sets each priority on itself and reads it back */
static
VOID
NTAPI
SetPriorityWorker(
    _In_ PVOID Parameter)
{
    PSET_PRIORITY_CONTEXT Context = Parameter;
    PKTHREAD Thread = KeGetCurrentThread();
    KPRIORITY Base = KeQueryPriorityThread(Thread);
    KPRIORITY Wanted[] = { 1, Base - 1, Base + 1, 15, 16, 31, Base };
    ULONG Index;

    Context->Base = Base;
    Context->Count = RTL_NUMBER_OF(Wanted);
    for (Index = 0; Index < Context->Count; Index++)
    {
        Context->Requested[Index] = Wanted[Index];
        Context->Returned[Index] = KeSetPriorityThread(Thread, Wanted[Index]);
        Context->Seen[Index] = KeQueryPriorityThread(Thread);
    }

    KeSetEvent(&Context->Done, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(KeSetPriorityThread)
{
    SET_PRIORITY_CONTEXT Context;
    OBJECT_ATTRIBUTES ObjectAttributes;
    LARGE_INTEGER Timeout;
    HANDLE Handle;
    NTSTATUS Status;
    KPRIORITY Previous;
    ULONG Index;

    RtlZeroMemory(&Context, sizeof(Context));
    KeInitializeEvent(&Context.Done, NotificationEvent, FALSE);
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, &ObjectAttributes, NULL, NULL, SetPriorityWorker, &Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Timeout.QuadPart = -10LL * 1000 * 1000 * 10;
    Status = KeWaitForSingleObject(&Context.Done, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwWaitForSingleObject(Handle, FALSE, &Timeout);
    ZwClose(Handle);
    if (!NT_SUCCESS(Status))
        return;

    ok(Context.Base >= 2 && Context.Base <= 14, "system thread base priority %ld\n", Context.Base);

    /* Each request is honoured and read back as such, below the base priority too; the old priority is the one returned */
    Previous = Context.Base;
    for (Index = 0; Index < Context.Count; Index++)
    {
        ok(Context.Seen[Index] == Context.Requested[Index],
           "request %ld read back as %ld (base %ld)\n", Context.Requested[Index], Context.Seen[Index], Context.Base);
        ok(Context.Returned[Index] == Previous,
           "request %ld returned old priority %ld, expected %ld\n", Context.Requested[Index], Context.Returned[Index], Previous);
        Previous = Context.Requested[Index];
    }
}
