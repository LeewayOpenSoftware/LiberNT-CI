/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test driver for IoAsynchronousPageWrite
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The driver's own device receives the paging write. It completes it at once
 * or later from a worker thread, with success or an error; each time the
 * caller's routine must run once, in the calling thread at APC_LEVEL, with its
 * context and the final status, and the IRP the device saw must be an
 * asynchronous paging write (IRP_PAGING_IO without IRP_SYNCHRONOUS_PAGING_IO).
 */

#include <kmt_test.h>
#include <ndk/iofuncs.h>

#define NDEBUG
#include <debug.h>

static KMT_IRP_HANDLER TestIrpHandler;

static KEVENT RoutineDone;
static ULONG RoutineCalls;
static PVOID RoutineContext;
static IO_STATUS_BLOCK RoutineIoStatus;
static KIRQL RoutineIrql;
static PETHREAD RoutineThread;

static ULONG PagingFlags;
static ULONG PagingLength;
static LONGLONG PagingOffset;
static NTSTATUS CompleteWith;
static BOOLEAN CompleteLater;
static WORK_QUEUE_ITEM LaterItem;

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"IoAsyncPageWrite";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CLOSE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}

static
VOID
NTAPI
WriteDone(
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

static
VOID
NTAPI
CompleteLaterWorker(
    _In_ PVOID Context)
{
    PIRP Irp = Context;

    Irp->IoStatus.Status = CompleteWith;
    Irp->IoStatus.Information = NT_SUCCESS(CompleteWith) ? PagingLength : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
VOID
RunCase(
    _In_ PFILE_OBJECT FileObject,
    _In_ NTSTATUS With,
    _In_ BOOLEAN Later,
    _In_ PCSTR Name)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset, Timeout;
    PUCHAR Buffer;
    NTSTATUS Status;
    PIRP Irp = NULL;
    PMDL Mdl;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'WpAK');
    ok(Buffer != NULL, "%s: no buffer\n", Name);
    if (Buffer == NULL)
        return;
    RtlFillMemory(Buffer, PAGE_SIZE, 0x5A);

    Mdl = IoAllocateMdl(Buffer, PAGE_SIZE, FALSE, FALSE, NULL);
    ok(Mdl != NULL, "%s: no MDL\n", Name);
    if (Mdl == NULL)
    {
        ExFreePoolWithTag(Buffer, 'WpAK');
        return;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    KeClearEvent(&RoutineDone);
    RoutineCalls = 0;
    RoutineContext = NULL;
    RoutineIrql = PASSIVE_LEVEL;
    RoutineThread = NULL;
    PagingFlags = 0;
    PagingLength = 0;
    PagingOffset = -1;
    CompleteWith = With;
    CompleteLater = Later;
    RtlFillMemory(&IoStatus, sizeof(IoStatus), 0x55);

    Offset.QuadPart = 2 * PAGE_SIZE;
    Status = IoAsynchronousPageWrite(FileObject, Mdl, &Offset, WriteDone, (PVOID)(ULONG_PTR)0xC0DE,
                                     IoPagingPriorityNormal, &IoStatus, &Irp);
    ok(Irp != NULL, "%s: no IRP returned\n", Name);
    ok(Status == (Later ? STATUS_PENDING : With), "%s: returned 0x%lx\n", Name, Status);

    Timeout.QuadPart = -10LL * 1000 * 1000;
    Status = KeWaitForSingleObject(&RoutineDone, Executive, KernelMode, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "%s: the completion routine did not run (0x%lx)\n", Name, Status);
    ok(RoutineCalls == 1, "%s: routine ran %lu times\n", Name, RoutineCalls);
    ok(RoutineContext == (PVOID)(ULONG_PTR)0xC0DE, "%s: wrong context %p\n", Name, RoutineContext);
    ok(RoutineIoStatus.Status == With, "%s: routine saw 0x%lx\n", Name, RoutineIoStatus.Status);
    ok(IoStatus.Status == With, "%s: status block holds 0x%lx\n", Name, IoStatus.Status);
    ok(RoutineIrql == APC_LEVEL, "%s: routine ran at IRQL %u\n", Name, RoutineIrql);
    ok(RoutineThread == PsGetCurrentThread(), "%s: routine ran in another thread\n", Name);

    ok(FlagOn(PagingFlags, IRP_PAGING_IO) && FlagOn(PagingFlags, IRP_NOCACHE) &&
       !FlagOn(PagingFlags, IRP_SYNCHRONOUS_PAGING_IO),
       "%s: the device saw IRP flags 0x%lx\n", Name, PagingFlags);
    ok(PagingLength == PAGE_SIZE && PagingOffset == 2 * PAGE_SIZE,
       "%s: the device saw length %lu offset %I64d\n", Name, PagingLength, PagingOffset);

    IoFreeMdl(Mdl);
    ExFreePoolWithTag(Buffer, 'WpAK');
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_WRITE && FlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        PagingFlags = Irp->Flags;
        PagingLength = IoStack->Parameters.Write.Length;
        PagingOffset = IoStack->Parameters.Write.ByteOffset.QuadPart;

        if (CompleteLater)
        {
            IoMarkIrpPending(Irp);
            ExInitializeWorkItem(&LaterItem, CompleteLaterWorker, Irp);
            ExQueueWorkItem(&LaterItem, DelayedWorkQueue);
            return STATUS_PENDING;
        }

        Status = CompleteWith;
        Irp->IoStatus.Information = NT_SUCCESS(Status) ? PagingLength : 0;
    }
    else if (IoStack->MajorFunction == IRP_MJ_WRITE)
    {
        /* The user-mode half writes once: run the cases from here */
        KeInitializeEvent(&RoutineDone, NotificationEvent, FALSE);
        RunCase(IoStack->FileObject, STATUS_SUCCESS, FALSE, "inline success");
        RunCase(IoStack->FileObject, STATUS_SUCCESS, TRUE, "later success");
        RunCase(IoStack->FileObject, STATUS_DEVICE_DATA_ERROR, FALSE, "inline error");
        Irp->IoStatus.Information = IoStack->Parameters.Write.Length;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
