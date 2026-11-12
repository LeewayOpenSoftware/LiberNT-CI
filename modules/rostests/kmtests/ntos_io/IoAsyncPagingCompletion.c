/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Completion of an asynchronous paging IRP (IRP_PAGING_IO without
 *              IRP_SYNCHRONOUS_PAGING_IO), the kind a page write issues
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The completion routine must run once, in the issuing thread, at APC_LEVEL,
 * with its context and the IRP's status; the I/O status block and the event
 * are set, and the IRP is freed. A kernel that does not implement this path
 * stops in a debug break here, so run it only on a kernel that does.
 */

#include <kmt_test.h>

static KEVENT RoutineDone;
static ULONG RoutineCalls;
static PVOID RoutineContext;
static IO_STATUS_BLOCK RoutineIoStatus;
static KIRQL RoutineIrql;
static PETHREAD RoutineThread;

static
VOID
NTAPI
PagingWriteDone(
    _In_ PVOID ApcContext,
    _In_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ ULONG Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    RoutineCalls++;
    RoutineContext = ApcContext;
    RoutineIoStatus = *IoStatusBlock;
    RoutineIrql = KeGetCurrentIrql();
    RoutineThread = PsGetCurrentThread();
    KeSetEvent(&RoutineDone, IO_NO_INCREMENT, FALSE);
}

START_TEST(IoAsyncPagingCompletion)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Timeout;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&RoutineDone, NotificationEvent, FALSE);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlFillMemory(&IoStatus, sizeof(IoStatus), 0x55);

    Irp = IoAllocateIrp(1, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (Irp == NULL)
        return;

    Irp->Flags = IRP_PAGING_IO | IRP_NOCACHE;
    Irp->RequestorMode = KernelMode;
    Irp->UserIosb = &IoStatus;
    Irp->UserEvent = &Event;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->Overlay.AsynchronousParameters.UserApcRoutine = PagingWriteDone;
    Irp->Overlay.AsynchronousParameters.UserApcContext = (PVOID)(ULONG_PTR)0x5A5A;
    IoSetNextIrpStackLocation(Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = PAGE_SIZE;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Timeout.QuadPart = -10LL * 1000 * 1000;
    Status = KeWaitForSingleObject(&RoutineDone, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(RoutineCalls, 1UL);
    ok_eq_pointer(RoutineContext, (PVOID)(ULONG_PTR)0x5A5A);
    ok_eq_hex(RoutineIoStatus.Status, STATUS_SUCCESS);
    ok_eq_ulongptr(RoutineIoStatus.Information, (ULONG_PTR)PAGE_SIZE);
    ok_eq_uint(RoutineIrql, APC_LEVEL);
    ok_eq_pointer(RoutineThread, PsGetCurrentThread());
    ok_eq_hex(IoStatus.Status, STATUS_SUCCESS);
    ok_eq_ulongptr(IoStatus.Information, (ULONG_PTR)PAGE_SIZE);
    ok(KeReadStateEvent(&Event) != 0, "the IRP's event was not signalled\n");
}
