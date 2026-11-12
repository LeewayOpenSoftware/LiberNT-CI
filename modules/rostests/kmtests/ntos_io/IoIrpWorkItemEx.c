/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for IoSetIoPriorityHint, IoTryQueueWorkItem, IoAllocateIrpEx and IoGetInitiatorProcess
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

/* IoGetIoPriorityHint and IoSetIoPriorityHint come from wdm.h */
BOOLEAN
NTAPI
IoTryQueueWorkItem(
    _Inout_ PIO_WORKITEM IoWorkItem,
    _In_ PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_opt_ PVOID Context);

PIRP
NTAPI
IoAllocateIrpEx(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ CCHAR StackSize,
    _In_ BOOLEAN ChargeQuota);

PEPROCESS
NTAPI
IoGetInitiatorProcess(
    _In_ PFILE_OBJECT FileObject);

#define TRY_ROUNDS 200

static KEVENT WorkGate;
static KEVENT WorkRan;
static volatile LONG WorkRuns;
static volatile LONG WorkBadArguments;
static PIO_WORKITEM ExpectedItem;
static PDEVICE_OBJECT ExpectedDevice;

static
VOID
TestPriorityHint(VOID)
{
    PIRP Irp;
    NTSTATUS Status;
    ULONG Hint;
    ULONG FlagsBefore;

    Irp = IoAllocateIrp(1, FALSE);
    ok(Irp != NULL, "IoAllocateIrp failed\n");
    if (Irp == NULL)
        return;

    /* A fresh IRP has no hint, which reads as normal */
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityNormal);

    for (Hint = IoPriorityVeryLow; Hint < MaxIoPriorityTypes; Hint++)
    {
        Status = IoSetIoPriorityHint(Irp, (IO_PRIORITY_HINT)Hint);
        ok(Status == STATUS_SUCCESS, "Hint %lu: status 0x%08lx\n", Hint, Status);
        ok(IoGetIoPriorityHint(Irp) == (IO_PRIORITY_HINT)Hint, "Hint %lu reads back as %d\n",
           Hint, IoGetIoPriorityHint(Irp));
    }

    /* A value that is not a hint is refused and changes nothing */
    Status = IoSetIoPriorityHint(Irp, IoPriorityLow);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = IoSetIoPriorityHint(Irp, MaxIoPriorityTypes);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityLow);

    /* The hint does not disturb the IRP's own flags, nor they the hint */
    Irp->Flags |= IRP_NOCACHE | IRP_SYNCHRONOUS_API | IRP_CLASS_CACHE_OPERATION;
    FlagsBefore = Irp->Flags;
    Status = IoSetIoPriorityHint(Irp, IoPriorityCritical);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_hex(Irp->Flags & 0xFFFF, FlagsBefore & 0xFFFF);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityCritical);

    IoFreeIrp(Irp);
}

static IO_WORKITEM_ROUTINE_EX TryWorkRoutine;

static
VOID
NTAPI
TryWorkRoutine(
    _In_ PVOID IoObject,
    _In_opt_ PVOID Context,
    _In_ PIO_WORKITEM IoWorkItem)
{
    if (IoObject != ExpectedDevice || Context != (PVOID)(ULONG_PTR)0xA1A1 || IoWorkItem != ExpectedItem)
        InterlockedIncrement(&WorkBadArguments);

    /* Hold the first run so that the caller can see the item running, not queued */
    KeWaitForSingleObject(&WorkGate, Executive, KernelMode, FALSE, NULL);

    InterlockedIncrement(&WorkRuns);
    KeSetEvent(&WorkRan, IO_NO_INCREMENT, FALSE);
}

static
VOID
TestTryQueueWorkItem(VOID)
{
    PDEVICE_OBJECT DeviceObject;
    PIO_WORKITEM IoItem;
    NTSTATUS Status;
    LARGE_INTEGER Timeout;
    LARGE_INTEGER Poll;
    KIRQL Irql;
    BOOLEAN First, Second;
    LONG Accepted = 0;
    ULONG i, Waited;

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    IoItem = IoAllocateWorkItem(DeviceObject);
    ok(IoItem != NULL, "IoAllocateWorkItem failed\n");
    if (IoItem == NULL)
    {
        IoDeleteDevice(DeviceObject);
        return;
    }

    ExpectedItem = IoItem;
    ExpectedDevice = DeviceObject;
    WorkRuns = 0;
    WorkBadArguments = 0;
    KeInitializeEvent(&WorkGate, NotificationEvent, TRUE);
    KeInitializeEvent(&WorkRan, SynchronizationEvent, FALSE);
    Timeout.QuadPart = -10 * 1000 * 1000 * 10;
    Poll.QuadPart = -10 * 1000 * 10;

    /* One queue, one run, with the three arguments the caller gave */
    First = IoTryQueueWorkItem(IoItem, TryWorkRoutine, DelayedWorkQueue, (PVOID)(ULONG_PTR)0xA1A1);
    ok_eq_bool(First, TRUE);
    Status = KeWaitForSingleObject(&WorkRan, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_long(WorkRuns, 1L);

    /* On one processor a worker cannot take the item while this thread stays at DISPATCH_LEVEL,
       so the second call must find it queued. With more processors either answer is possible. */
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    First = IoTryQueueWorkItem(IoItem, TryWorkRoutine, DelayedWorkQueue, (PVOID)(ULONG_PTR)0xA1A1);
    Second = IoTryQueueWorkItem(IoItem, TryWorkRoutine, DelayedWorkQueue, (PVOID)(ULONG_PTR)0xA1A1);
    KeLowerIrql(Irql);
    ok_eq_bool(First, TRUE);
    Accepted = 1 + (First ? 1 : 0) + (Second ? 1 : 0);
    if (KeNumberProcessors == 1)
        ok_eq_bool(Second, FALSE);

    /* Whatever the timing, every accepted call gives exactly one run */
    for (i = 0; i < TRY_ROUNDS; i++)
    {
        if (IoTryQueueWorkItem(IoItem, TryWorkRoutine, DelayedWorkQueue, (PVOID)(ULONG_PTR)0xA1A1))
            Accepted++;
    }

    for (Waited = 0; Waited < 10000 && WorkRuns != Accepted; Waited++)
        KeDelayExecutionThread(KernelMode, FALSE, &Poll);

    ok_eq_long(WorkRuns, Accepted);
    ok_eq_long(WorkBadArguments, 0L);

    /* Let a late run, if the check above failed, finish before the item goes away */
    KeDelayExecutionThread(KernelMode, FALSE, &Poll);
    IoFreeWorkItem(IoItem);
    IoDeleteDevice(DeviceObject);
}

static
VOID
TestAllocateIrpEx(VOID)
{
    PIRP Irp;

    Irp = IoAllocateIrpEx(NULL, 3, FALSE);
    ok(Irp != NULL, "IoAllocateIrpEx failed\n");
    if (Irp == NULL)
        return;

    ok_eq_int(Irp->StackCount, 3);
    ok_eq_int(Irp->CurrentLocation, 4);
    ok_eq_uint(Irp->Type, IO_TYPE_IRP);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityNormal);
    IoFreeIrp(Irp);
}

static
VOID
TestInitiatorProcess(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Null");
    PFILE_OBJECT FileObject = NULL;
    PDEVICE_OBJECT DeviceObject = NULL;
    NTSTATUS Status;

    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_ATTRIBUTES, &FileObject, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Nobody opened this file on behalf of another process */
    ok_eq_pointer(IoGetInitiatorProcess(FileObject), NULL);
    ObDereferenceObject(FileObject);
}

START_TEST(IoIrpWorkItemEx)
{
    TestPriorityHint();
    TestTryQueueWorkItem();
    TestAllocateIrpEx();
    TestInitiatorProcess();
}
