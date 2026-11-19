/*
 * PROJECT:     LiberNT NTFS driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USN change journal FSCTLs and the queue of reads waiting for records
 */

#include "ntfspch.h"

/* Records are staged in pool and copied to the caller after the metadata lock is dropped */
#define NTFS_USN_STAGING_MAX    (64 * 1024)
/* Journal bytes one read looks at, so a filter that matches little holds no lock for long */
#define NTFS_USN_READ_SCAN_MAX  (1024 * 1024)
/* USN_RECORD_V4 keeps its reason right after the 128-bit references and the USN */
#define NTFS_USN_V4_REASON      0x30

/*
 * The journal FSCTLs need a volume handle opened for data access; Windows fails them with
 * an invalid-function error on a volume handle opened without read or write access.
 */
static
NTSTATUS
NtfsUsnGetVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIO_STACK_LOCATION IrpSp,
    _Out_ PVolumeContextBlock* VolCB,
    _Out_ PFileContextBlock* FileCB)
{
    *VolCB = VolumeDeviceObject ? (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension : NULL;
    *FileCB = NtfsGetFileContext(IrpSp->FileObject);
    if (!*VolCB || !(*VolCB)->DiskVolume || !*FileCB || !(*FileCB)->IsVolumeOpen)
        return STATUS_INVALID_PARAMETER;
    if (!((*FileCB)->DesiredAccess & (FILE_READ_DATA | FILE_WRITE_DATA)))
        return STATUS_INVALID_DEVICE_REQUEST;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsUsnCreate(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    CREATE_USN_JOURNAL_DATA Input;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    PVOID InputBuffer = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    LARGE_INTEGER Now;
    NTSTATUS Status;

    Status = NtfsUsnGetVolume(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!(FileCB->DesiredAccess & FILE_WRITE_DATA) && !FileCB->ManageVolumeAccess)
        return STATUS_ACCESS_DENIED;
    if (NtfsVolumeIsReadOnly(VolCB->DiskVolume))
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (IrpSp->Parameters.FileSystemControl.InputBufferLength < sizeof(Input) || !InputBuffer)
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
            ProbeForRead(InputBuffer, sizeof(Input), sizeof(UCHAR));
        RtlCopyMemory(&Input, InputBuffer, sizeof(Input));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;

    /* The identifier of a new journal is its creation time */
    KeQuerySystemTime(&Now);
    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnJournalCreate(VolCB->DiskVolume,
                                  Input.MaximumSize,
                                  Input.AllocationDelta,
                                  (ULONGLONG)Now.QuadPart);
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();
    return Status;
}

static
NTSTATUS
NtfsUsnQuery(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    PUSN_JOURNAL_DATA_V2 Output = (PUSN_JOURNAL_DATA_V2)Irp->AssociatedIrp.SystemBuffer;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    NtfsUsnJournalData Data;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    NTSTATUS Status;

    Status = NtfsUsnGetVolume(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!Output || OutputLength < sizeof(USN_JOURNAL_DATA_V0))
        return STATUS_BUFFER_TOO_SMALL;

    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnJournalQuery(VolCB->DiskVolume, &Data);
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();
    if (!NT_SUCCESS(Status))
        return Status;

    /* The largest structure that fits; only version 2 records are written */
    RtlZeroMemory(Output, min(OutputLength, (ULONG)sizeof(USN_JOURNAL_DATA_V2)));
    Output->UsnJournalID = Data.UsnJournalId;
    Output->FirstUsn = Data.FirstUsn;
    Output->NextUsn = Data.NextUsn;
    Output->LowestValidUsn = Data.LowestValidUsn;
    Output->MaxUsn = Data.MaxUsn;
    Output->MaximumSize = Data.MaximumSize;
    Output->AllocationDelta = Data.AllocationDelta;
    Irp->IoStatus.Information = sizeof(USN_JOURNAL_DATA_V0);
    if (OutputLength >= sizeof(USN_JOURNAL_DATA_V1))
    {
        Output->MinSupportedMajorVersion = 2;
        Output->MaxSupportedMajorVersion = 2;
        Irp->IoStatus.Information = sizeof(USN_JOURNAL_DATA_V1);
    }
    if (OutputLength >= sizeof(USN_JOURNAL_DATA_V2))
        Irp->IoStatus.Information = sizeof(USN_JOURNAL_DATA_V2);
    return STATUS_SUCCESS;
}

/* Whether a record passes the caller's version range and reason filter */
static
BOOLEAN
NtfsUsnRecordSelected(
    _In_ const UCHAR* Record,
    _In_ const READ_USN_JOURNAL_DATA_V1* Input)
{
    const USN_RECORD_COMMON_HEADER* Header = (const USN_RECORD_COMMON_HEADER*)Record;
    ULONG ReasonOffset;
    ULONG Reason;

    if (Header->MajorVersion < Input->MinMajorVersion || Header->MajorVersion > Input->MaxMajorVersion)
        return FALSE;
    if (Header->MajorVersion == 2)
        ReasonOffset = FIELD_OFFSET(USN_RECORD_V2, Reason);
    else if (Header->MajorVersion == 3)
        ReasonOffset = FIELD_OFFSET(USN_RECORD_V3, Reason);
    else
        ReasonOffset = NTFS_USN_V4_REASON;
    if (Header->RecordLength < ReasonOffset + sizeof(ULONG))
        return FALSE;
    RtlCopyMemory(&Reason, Record + ReasonOffset, sizeof(Reason));
    if (!(Reason & Input->ReasonMask))
        return FALSE;
    if (Input->ReturnOnlyOnClose && !(Reason & USN_REASON_CLOSE))
        return FALSE;
    return TRUE;
}

/*
 * Reads records for one request into Staging: the next USN in Cursor, the journal's next USN
 * in JournalNext. Filtered-out records still advance Cursor. One call looks at a bounded
 * amount of journal and may return nothing with the USN to go on from.
 */
static
NTSTATUS
NtfsUsnCollect(
    _In_ PVolumeContextBlock VolCB,
    _In_ const READ_USN_JOURNAL_DATA_V1* Input,
    _Out_writes_bytes_(Capacity) PUCHAR Staging,
    _In_ ULONG Capacity,
    _Inout_updates_bytes_(Capacity) PUCHAR Raw,
    _Out_ PULONG Filled,
    _Out_ LONGLONG* Cursor,
    _Out_ LONGLONG* JournalNext)
{
    NtfsUsnJournalData Data;
    ULONGLONG Scanned = 0;
    ULONG RawLength, Offset;
    LONGLONG Next;
    NTSTATUS Status;

    *Filled = 0;
    *Cursor = Input->StartUsn;
    *JournalNext = 0;

    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnJournalQuery(VolCB->DiskVolume, &Data);
    if (NT_SUCCESS(Status) && Data.UsnJournalId != Input->UsnJournalID)
        Status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(Status))
        *JournalNext = Data.NextUsn;

    while (NT_SUCCESS(Status) && *Filled < Capacity && Scanned < NTFS_USN_READ_SCAN_MAX)
    {
        Status = NtfsUsnJournalRead(VolCB->DiskVolume, *Cursor, Raw, Capacity - *Filled, &RawLength, &Next);
        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            /* The next record does not fit what is left of the caller's buffer */
            Status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(Status))
            break;

        for (Offset = 0; Offset + sizeof(USN_RECORD_COMMON_HEADER) <= RawLength;)
        {
            const USN_RECORD_COMMON_HEADER* Header = (const USN_RECORD_COMMON_HEADER*)(Raw + Offset);

            if (NtfsUsnRecordSelected(Raw + Offset, Input))
            {
                RtlCopyMemory(Staging + *Filled, Raw + Offset, Header->RecordLength);
                *Filled += Header->RecordLength;
            }
            Offset += Header->RecordLength;
        }
        if (Next > *Cursor)
            Scanned += (ULONGLONG)(Next - *Cursor);
        *Cursor = Next;
        if (RawLength == 0)
            break;
    }
    /* 0 asks for the first record; report the real USN reading continues from */
    if (NT_SUCCESS(Status) && *Cursor == 0)
        *Cursor = Data.FirstUsn;
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();
    return Status;
}

/* Per-request wait state, in Irp->Tail.Overlay.DriverContext[0] (the CSQ owns [3]) */
typedef struct _NTFS_USN_WAIT
{
    PVolumeContextBlock VolCB;
    READ_USN_JOURNAL_DATA_V1 Input;
    ULONG OutputLength;
    LONGLONG ThresholdUsn;          /* journal next USN at which BytesToWaitFor is met */
    ULONGLONG Deadline;             /* interrupt time when Timeout elapses, 0 for none */
} NTFS_USN_WAIT, *PNTFS_USN_WAIT;

#define NTFS_USN_WAIT_TAG       'wUtN'
#define NTFS_USN_TIMER_PERIOD   1000    /* ms between Timeout checks while someone waits */
/* A larger Timeout (seconds) would overflow the interrupt-time deadline: treat it as none */
#define NTFS_USN_MAX_TIMEOUT    0x7FFFFFFFULL

static
VOID
NtfsUsnCompleteWait(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    PNTFS_USN_WAIT Wait = (PNTFS_USN_WAIT)Irp->Tail.Overlay.DriverContext[0];

    Irp->Tail.Overlay.DriverContext[0] = NULL;
    if (Wait)
    {
        if (Wait->Deadline)
            InterlockedDecrement(&Wait->VolCB->UsnDeadlineWaiters);
        ExFreePoolWithTag(Wait, NTFS_USN_WAIT_TAG);
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = NT_SUCCESS(Status) ? Information : 0;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
}

static
VOID
NTAPI
NtfsUsnCsqInsert(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    PVolumeContextBlock VolCB = CONTAINING_RECORD(Csq, VolumeContextBlock, UsnWaitCsq);

    InsertTailList(&VolCB->UsnWaitList, &Irp->Tail.Overlay.ListEntry);
}

static
VOID
NTAPI
NtfsUsnCsqRemove(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

/* PeekContext, when given, is the file object whose requests are wanted */
static
PIRP
NTAPI
NtfsUsnCsqPeekNext(
    _In_ PIO_CSQ Csq,
    _In_opt_ PIRP Irp,
    _In_opt_ PVOID PeekContext)
{
    PVolumeContextBlock VolCB = CONTAINING_RECORD(Csq, VolumeContextBlock, UsnWaitCsq);
    PLIST_ENTRY Entry = Irp ? Irp->Tail.Overlay.ListEntry.Flink : VolCB->UsnWaitList.Flink;

    for (; Entry != &VolCB->UsnWaitList; Entry = Entry->Flink)
    {
        PIRP Next = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (!PeekContext || IoGetCurrentIrpStackLocation(Next)->FileObject == (PFILE_OBJECT)PeekContext)
            return Next;
    }
    return NULL;
}

static
VOID
NTAPI
NtfsUsnCsqAcquire(
    _In_ PIO_CSQ Csq,
    _Out_ PKIRQL Irql)
{
    PVolumeContextBlock VolCB = CONTAINING_RECORD(Csq, VolumeContextBlock, UsnWaitCsq);

    KeAcquireSpinLock(&VolCB->UsnWaitLock, Irql);
}

static
VOID
NTAPI
NtfsUsnCsqRelease(
    _In_ PIO_CSQ Csq,
    _In_ KIRQL Irql)
{
    PVolumeContextBlock VolCB = CONTAINING_RECORD(Csq, VolumeContextBlock, UsnWaitCsq);

    KeReleaseSpinLock(&VolCB->UsnWaitLock, Irql);
}

static
VOID
NTAPI
NtfsUsnCsqCompleteCanceled(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    NtfsUsnCompleteWait(Irp, STATUS_CANCELLED, 0);
}

static
VOID
NTAPI
NtfsUsnWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context);

/* Queues one re-check of the waiting reads; repeated calls before it runs coalesce */
VOID
NtfsUsnWakeWaiters(
    _In_ PVolumeContextBlock VolCB)
{
    PIO_WORKITEM Item;

    if (!VolCB->UsnDeviceObject || VolCB->UsnShuttingDown)
        return;
    /* Released by the worker; volume teardown waits for it */
    if (!ExAcquireRundownProtection(&VolCB->UsnRundown))
        return;
    if (InterlockedCompareExchange(&VolCB->UsnWakeQueued, 1, 0) != 0)
    {
        ExReleaseRundownProtection(&VolCB->UsnRundown);
        return;
    }
    Item = IoAllocateWorkItem(VolCB->UsnDeviceObject);
    if (!Item)
    {
        InterlockedExchange(&VolCB->UsnWakeQueued, 0);
        ExReleaseRundownProtection(&VolCB->UsnRundown);
        return;
    }
    IoQueueWorkItem(Item, NtfsUsnWakeWorker, DelayedWorkQueue, Item);
}

static
VOID
NTAPI
NtfsUsnTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);
    NtfsUsnWakeWaiters((PVolumeContextBlock)Context);
}

static
VOID
NtfsUsnArmTimer(
    _In_ PVolumeContextBlock VolCB)
{
    LARGE_INTEGER Due;
    KIRQL Irql;

    KeAcquireSpinLock(&VolCB->UsnWaitLock, &Irql);
    if (!VolCB->UsnWaitTimerArmed && !VolCB->UsnShuttingDown)
    {
        Due.QuadPart = -10000LL * NTFS_USN_TIMER_PERIOD;
        KeSetTimerEx(&VolCB->UsnWaitTimer, Due, NTFS_USN_TIMER_PERIOD, &VolCB->UsnWaitTimerDpc);
        VolCB->UsnWaitTimerArmed = TRUE;
    }
    KeReleaseSpinLock(&VolCB->UsnWaitLock, Irql);
}

/*
 * Re-checks every waiting read at PASSIVE_LEVEL. A read completes when records match and
 * either BytesToWaitFor bytes were added since it started waiting or its Timeout elapsed;
 * otherwise it goes back to the queue, starting from where the last check stopped.
 */
static
VOID
NTAPI
NtfsUsnWakeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PVolumeContextBlock VolCB = (PVolumeContextBlock)DeviceObject->DeviceExtension;
    PIO_WORKITEM Item = (PIO_WORKITEM)Context;
    PUCHAR Staging = NULL, Raw = NULL;
    BOOLEAN KeptAny = FALSE;
    LIST_ENTRY Keep;
    PIRP Irp;
    KIRQL Irql;

    LONG CleanupSeq = VolCB->UsnCleanupSeq;

    /* A wake that arrives from here on queues another pass */
    InterlockedExchange(&VolCB->UsnWakeQueued, 0);
    InitializeListHead(&Keep);
    Staging = ExAllocatePoolWithTag(PagedPool, NTFS_USN_STAGING_MAX, TAG_NTFS);
    Raw = ExAllocatePoolWithTag(PagedPool, NTFS_USN_STAGING_MAX, TAG_NTFS);

    while ((Irp = IoCsqRemoveNextIrp(&VolCB->UsnWaitCsq, NULL)) != NULL)
    {
        PNTFS_USN_WAIT Wait = (PNTFS_USN_WAIT)Irp->Tail.Overlay.DriverContext[0];
        ULONG Capacity, Filled = 0;
        LONGLONG Cursor = 0, JournalNext = 0;
        PUCHAR Output;
        BOOLEAN TimedOut;
        NTSTATUS Status;

        if (VolCB->Dismounted || VolCB->UsnShuttingDown)
        {
            NtfsUsnCompleteWait(Irp, STATUS_VOLUME_DISMOUNTED, 0);
            continue;
        }
        /* The handle was cleaned up while this request was out of the queue */
        if (IoGetCurrentIrpStackLocation(Irp)->FileObject->Flags & FO_CLEANUP_COMPLETE)
        {
            NtfsUsnCompleteWait(Irp, STATUS_CANCELLED, 0);
            continue;
        }
        if (!Staging || !Raw)
        {
            NtfsUsnCompleteWait(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            continue;
        }
        Output = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!Output)
        {
            NtfsUsnCompleteWait(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            continue;
        }

        Capacity = min(Wait->OutputLength - (ULONG)sizeof(USN), (ULONG)NTFS_USN_STAGING_MAX);
        Status = NtfsUsnCollect(VolCB, &Wait->Input, Staging, Capacity, Raw, &Filled, &Cursor, &JournalNext);
        if (!NT_SUCCESS(Status))
        {
            /* The journal was deleted or replaced, or reading failed */
            NtfsUsnCompleteWait(Irp, Status, 0);
            continue;
        }

        /* Timeout gives the caller control back: it returns even with no record, just the next USN */
        TimedOut = Wait->Deadline && KeQueryInterruptTime() >= Wait->Deadline;
        if (TimedOut || (Filled && JournalNext >= Wait->ThresholdUsn))
        {
            RtlCopyMemory(Output, &Cursor, sizeof(Cursor));
            if (Filled)
                RtlCopyMemory(Output + sizeof(USN), Staging, Filled);
            NtfsUsnCompleteWait(Irp, STATUS_SUCCESS, sizeof(USN) + Filled);
            continue;
        }

        /* Keep waiting; filtered-out records are not looked at again */
        if (!Filled)
            Wait->Input.StartUsn = Cursor;
        if (JournalNext >= Wait->ThresholdUsn)
            Wait->ThresholdUsn = JournalNext + (LONGLONG)Wait->Input.BytesToWaitFor;
        InsertTailList(&Keep, &Irp->Tail.Overlay.ListEntry);
    }

    while (!IsListEmpty(&Keep))
    {
        Irp = CONTAINING_RECORD(RemoveHeadList(&Keep), IRP, Tail.Overlay.ListEntry);
        if (VolCB->UsnShuttingDown)
        {
            NtfsUsnCompleteWait(Irp, STATUS_VOLUME_DISMOUNTED, 0);
            continue;
        }
        KeptAny = TRUE;
        /* A request cancelled while it was out of the queue completes here as cancelled */
        IoCsqInsertIrp(&VolCB->UsnWaitCsq, Irp, NULL);
    }

    /* The timer runs only while a queued read has a Timeout; decided under the lock */
    KeAcquireSpinLock(&VolCB->UsnWaitLock, &Irql);
    if (VolCB->UsnWaitTimerArmed && VolCB->UsnDeadlineWaiters == 0)
    {
        KeCancelTimer(&VolCB->UsnWaitTimer);
        VolCB->UsnWaitTimerArmed = FALSE;
    }
    KeReleaseSpinLock(&VolCB->UsnWaitLock, Irql);

    if (Staging)
        ExFreePoolWithTag(Staging, TAG_NTFS);
    if (Raw)
        ExFreePoolWithTag(Raw, TAG_NTFS);
    IoFreeWorkItem(Item);
    /* A handle cleaned up while its request was held here: one more pass completes it */
    if (KeptAny && VolCB->UsnCleanupSeq != CleanupSeq)
        NtfsUsnWakeWaiters(VolCB);
    ExReleaseRundownProtection(&VolCB->UsnRundown);
}

VOID
NtfsUsnInitializeVolume(
    _In_ PVolumeContextBlock VolCB,
    _In_ PDEVICE_OBJECT VolumeDeviceObject)
{
    InitializeListHead(&VolCB->UsnWaitList);
    KeInitializeSpinLock(&VolCB->UsnWaitLock);
    IoCsqInitialize(&VolCB->UsnWaitCsq,
                    NtfsUsnCsqInsert,
                    NtfsUsnCsqRemove,
                    NtfsUsnCsqPeekNext,
                    NtfsUsnCsqAcquire,
                    NtfsUsnCsqRelease,
                    NtfsUsnCsqCompleteCanceled);
    KeInitializeTimerEx(&VolCB->UsnWaitTimer, NotificationTimer);
    KeInitializeDpc(&VolCB->UsnWaitTimerDpc, NtfsUsnTimerDpc, VolCB);
    VolCB->UsnWaitTimerArmed = FALSE;
    VolCB->UsnWakeQueued = 0;
    VolCB->UsnShuttingDown = FALSE;
    VolCB->UsnDeadlineWaiters = 0;
    VolCB->UsnCleanupSeq = 0;
    ExInitializeRundownProtection(&VolCB->UsnRundown);
    VolCB->UsnDeviceObject = VolumeDeviceObject;
}

/* Volume teardown: nothing may wait or tick on a volume that is going away */
VOID
NtfsUsnShutdownVolume(
    _In_ PVolumeContextBlock VolCB)
{
    PIRP Irp;
    KIRQL Irql;

    if (!VolCB->UsnDeviceObject)
        return;
    KeAcquireSpinLock(&VolCB->UsnWaitLock, &Irql);
    VolCB->UsnShuttingDown = TRUE;
    KeCancelTimer(&VolCB->UsnWaitTimer);
    VolCB->UsnWaitTimerArmed = FALSE;
    KeReleaseSpinLock(&VolCB->UsnWaitLock, Irql);
    KeFlushQueuedDpcs();

    /* No wake worker or queuing read is left after this; then nothing can re-queue */
    ExWaitForRundownProtectionRelease(&VolCB->UsnRundown);
    while ((Irp = IoCsqRemoveNextIrp(&VolCB->UsnWaitCsq, NULL)) != NULL)
        NtfsUsnCompleteWait(Irp, STATUS_VOLUME_DISMOUNTED, 0);
}

/* IRP_MJ_CLEANUP of a handle ends the journal reads still waiting on it */
VOID
NtfsUsnCancelFileWaiters(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PFILE_OBJECT FileObject)
{
    PVolumeContextBlock VolCB = VolumeDeviceObject ? (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension : NULL;
    PIRP Irp;

    if (!VolCB || !VolCB->UsnDeviceObject || !FileObject)
        return;
    InterlockedIncrement(&VolCB->UsnCleanupSeq);
    while ((Irp = IoCsqRemoveNextIrp(&VolCB->UsnWaitCsq, FileObject)) != NULL)
        NtfsUsnCompleteWait(Irp, STATUS_CANCELLED, 0);
}

static
NTSTATUS
NtfsUsnRead(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    READ_USN_JOURNAL_DATA_V1 Input;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    PVOID InputBuffer = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    ULONG InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PUCHAR Output;
    PUCHAR Staging = NULL;
    PUCHAR Raw = NULL;
    ULONG CopyLength, Capacity, Filled = 0;
    BOOLEAN IsV1;
    LONGLONG Cursor = 0, JournalNext = 0;
    PNTFS_USN_WAIT Wait;
    NTSTATUS Status;

    Status = NtfsUsnGetVolume(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;

    Output = GetUserBuffer(Irp);
    if (!InputBuffer || InputLength < sizeof(READ_USN_JOURNAL_DATA_V0))
        return STATUS_INVALID_PARAMETER;
    if (!Output || OutputLength < sizeof(USN))
        return STATUS_BUFFER_TOO_SMALL;

    /* V1 adds the version range at the end of the V0 layout */
    IsV1 = InputLength >= FIELD_OFFSET(READ_USN_JOURNAL_DATA_V1, MaxMajorVersion) + sizeof(USHORT);
    CopyLength = IsV1 ? min(InputLength, (ULONG)sizeof(Input)) : sizeof(READ_USN_JOURNAL_DATA_V0);
    RtlZeroMemory(&Input, sizeof(Input));
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
        {
            ProbeForRead(InputBuffer, CopyLength, sizeof(UCHAR));
            ProbeForWrite(Output, OutputLength, sizeof(UCHAR));
        }
        RtlCopyMemory(&Input, InputBuffer, CopyLength);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;

    if (!IsV1)
    {
        Input.MinMajorVersion = 2;
        Input.MaxMajorVersion = 2;
    }
    if (Input.MinMajorVersion < 2 || Input.MinMajorVersion > Input.MaxMajorVersion)
        return STATUS_INVALID_PARAMETER;
    if (Input.MaxMajorVersion > 4)
        Input.MaxMajorVersion = 4;
    /* A USN given by the caller is a record offset: never treat a bad one as corruption */
    if (Input.StartUsn < 0 || (Input.StartUsn & 7))
        return STATUS_INVALID_PARAMETER;

    Capacity = min(OutputLength - (ULONG)sizeof(USN), (ULONG)NTFS_USN_STAGING_MAX);
    if (Capacity)
    {
        Staging = ExAllocatePoolWithTag(PagedPool, Capacity, TAG_NTFS);
        Raw = ExAllocatePoolWithTag(PagedPool, Capacity, TAG_NTFS);
        if (!Staging || !Raw)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
    }

    Status = NtfsUsnCollect(VolCB, &Input, Staging, Capacity, Raw, &Filled, &Cursor, &JournalNext);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /*
     * Nothing to return and the caller asked to wait: queue the request. Its output buffer
     * is locked now, while this is still the caller's thread, and filled by the wake worker.
     */
    if (!Filled && Input.BytesToWaitFor && Capacity)
    {
        Wait = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Wait), NTFS_USN_WAIT_TAG);
        if (!Wait)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        Wait->VolCB = VolCB;
        Wait->Input = Input;
        Wait->Input.StartUsn = Cursor;
        Wait->OutputLength = OutputLength;
        Wait->ThresholdUsn = JournalNext + (LONGLONG)Input.BytesToWaitFor;
        /* Timeout is ignored for asynchronously opened handles */
        Wait->Deadline = (Input.Timeout && Input.Timeout <= NTFS_USN_MAX_TIMEOUT &&
                          (IrpSp->FileObject->Flags & FO_SYNCHRONOUS_IO))
                       ? KeQueryInterruptTime() + Input.Timeout * 10000000ULL : 0;

        if (!IoAllocateMdl(Output, OutputLength, FALSE, FALSE, Irp))
        {
            ExFreePoolWithTag(Wait, NTFS_USN_WAIT_TAG);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        _SEH2_TRY
        {
            MmProbeAndLockPages(Irp->MdlAddress, Irp->RequestorMode, IoWriteAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
        {
            IoFreeMdl(Irp->MdlAddress);
            Irp->MdlAddress = NULL;
            ExFreePoolWithTag(Wait, NTFS_USN_WAIT_TAG);
            goto Cleanup;
        }

        /* Teardown waits for this insert, then drains it; once it has started, refuse */
        if (!ExAcquireRundownProtection(&VolCB->UsnRundown))
        {
            MmUnlockPages(Irp->MdlAddress);
            IoFreeMdl(Irp->MdlAddress);
            Irp->MdlAddress = NULL;
            ExFreePoolWithTag(Wait, NTFS_USN_WAIT_TAG);
            Status = STATUS_VOLUME_DISMOUNTED;
            goto Cleanup;
        }
        Irp->Tail.Overlay.DriverContext[0] = Wait;
        if (Wait->Deadline)
        {
            InterlockedIncrement(&VolCB->UsnDeadlineWaiters);
            NtfsUsnArmTimer(VolCB);
        }
        /* Marks the IRP pending; one cancelled meanwhile is completed by the queue */
        IoCsqInsertIrp(&VolCB->UsnWaitCsq, Irp, NULL);
        /* Records appended since the check above must not be missed */
        NtfsUsnWakeWaiters(VolCB);
        ExReleaseRundownProtection(&VolCB->UsnRundown);
        Status = STATUS_PENDING;
        goto Cleanup;
    }

    _SEH2_TRY
    {
        RtlCopyMemory(Output, &Cursor, sizeof(Cursor));
        if (Filled)
            RtlCopyMemory(Output + sizeof(USN), Staging, Filled);
        Irp->IoStatus.Information = sizeof(USN) + Filled;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

Cleanup:
    if (Staging)
        ExFreePoolWithTag(Staging, TAG_NTFS);
    if (Raw)
        ExFreePoolWithTag(Raw, TAG_NTFS);
    return Status;
}

/* What a journal record says about a file: references, first long name, attributes */
typedef struct _NTFS_USN_FILE_DESCRIPTION
{
    ULONGLONG FileReference;
    ULONGLONG ParentReference;
    UNICODE_STRING Name;            /* points into the file record */
    ULONG FileAttributes;
    ULONG SecurityId;
    LONGLONG ChangeTime;
    LONGLONG StoredUsn;
} NTFS_USN_FILE_DESCRIPTION, *PNTFS_USN_FILE_DESCRIPTION;

static
NTSTATUS
NtfsUsnDescribeFile(
    _In_ PVolumeContextBlock VolCB,
    _In_ PNtfsFileRecord FileRec,
    _Out_ PNTFS_USN_FILE_DESCRIPTION Desc)
{
    PFileRecordHeader Header = NtfsFileRecordGetHeader(FileRec);
    NtfsFileBasicInformation Basic;
    ULONG Context = 0;
    NTSTATUS Status;

    RtlZeroMemory(Desc, sizeof(*Desc));
    Desc->FileReference = ((ULONGLONG)Header->SequenceNumber << 48) | Header->MFTRecordNumber;
    Status = NtfsMasterFileTableEnumerateLinks(NtfsVolumeGetMft(VolCB->DiskVolume),
                                               FileRec,
                                               &Context,
                                               &Desc->ParentReference,
                                               &Desc->Name);
    if (Status == STATUS_NO_MORE_ENTRIES)
    {
        RtlZeroMemory(&Desc->Name, sizeof(Desc->Name));
        Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status))
        Status = NtfsFileRecordGetBasicInformation(FileRec, &Basic);
    if (NT_SUCCESS(Status))
        Status = NtfsFileRecordGetUsnInformation(FileRec, &Desc->StoredUsn, &Desc->SecurityId);
    if (NT_SUCCESS(Status))
    {
        Desc->FileAttributes = Basic.FileAttributes;
        Desc->ChangeTime = (LONGLONG)Basic.ChangeTime;
    }
    return Status;
}

/* A file handle (the per-file journal FSCTLs do not take a volume handle) */
static
NTSTATUS
NtfsUsnGetFile(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIO_STACK_LOCATION IrpSp,
    _Out_ PVolumeContextBlock* VolCB,
    _Out_ PFileContextBlock* FileCB)
{
    *VolCB = VolumeDeviceObject ? (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension : NULL;
    *FileCB = NtfsGetFileContext(IrpSp->FileObject);
    if (!*VolCB || !(*VolCB)->DiskVolume || !*FileCB || !(*FileCB)->FileRec || (*FileCB)->IsVolumeOpen)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsUsnWriteCloseRecord(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    NTFS_USN_FILE_DESCRIPTION Desc;
    NtfsUsnRecordInput Input;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    LARGE_INTEGER Now;
    PUCHAR Output;
    LONGLONG Usn = 0;
    NTSTATUS Status;

    Status = NtfsUsnGetFile(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    Output = GetUserBuffer(Irp);
    if (!Output || OutputLength < sizeof(USN))
        return STATUS_BUFFER_TOO_SMALL;
    if (NtfsVolumeIsReadOnly(VolCB->DiskVolume))
        return STATUS_MEDIA_WRITE_PROTECTED;
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
            ProbeForWrite(Output, sizeof(USN), sizeof(UCHAR));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;

    KeQuerySystemTime(&Now);
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(NtfsGetMainResource(FileCB), TRUE);
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnDescribeFile(VolCB, FileCB->FileRec, &Desc);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(&Input, sizeof(Input));
        Input.FileReferenceNumber = Desc.FileReference;
        Input.ParentFileReferenceNumber = Desc.ParentReference;
        Input.TimeStamp = (ULONGLONG)Now.QuadPart;
        Input.Reason = USN_REASON_CLOSE;
        Input.SourceInfo = FileCB->UsnSourceInfo;
        Input.SecurityId = Desc.SecurityId;
        Input.FileAttributes = Desc.FileAttributes;
        Input.FileName = Desc.Name.Buffer;
        Input.FileNameLength = Desc.Name.Length;
        Status = NtfsUsnJournalAppend(VolCB->DiskVolume, &Input, &Usn);
    }
    /*
     * The name points into the record: it is consumed above, before the record can change.
     * Once appended the close record is durable, so its USN is returned even if stamping
     * $STANDARD_INFORMATION fails; the file then keeps its previous USN.
     */
    if (NT_SUCCESS(Status))
        (VOID)NtfsFileRecordSetUpdateSequenceNumber(FileCB->FileRec, Usn);
    NtfsReleaseMetadata(VolCB);
    ExReleaseResourceLite(NtfsGetMainResource(FileCB));
    KeLeaveCriticalRegion();
    if (!NT_SUCCESS(Status))
        return Status;
    NtfsUsnWakeWaiters(VolCB);

    _SEH2_TRY
    {
        RtlCopyMemory(Output, &Usn, sizeof(Usn));
        Irp->IoStatus.Information = sizeof(USN);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

static
NTSTATUS
NtfsUsnReadFileData(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    PVOID InputBuffer = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    ULONG InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    READ_FILE_USN_DATA Versions;
    NTFS_USN_FILE_DESCRIPTION Desc;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    PUCHAR Output, Record = NULL;
    ULONG HeaderLength = 0, Length = 0;
    USHORT Version;
    NTSTATUS Status;

    Status = NtfsUsnGetFile(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    Output = GetUserBuffer(Irp);
    if (!Output)
        return STATUS_INVALID_USER_BUFFER;

    /* The version range is optional; without it the record is version 2 */
    Versions.MinMajorVersion = 2;
    Versions.MaxMajorVersion = 2;
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
        {
            if (InputBuffer && InputLength >= sizeof(Versions))
                ProbeForRead(InputBuffer, sizeof(Versions), sizeof(UCHAR));
            ProbeForWrite(Output, OutputLength, sizeof(UCHAR));
        }
        if (InputBuffer && InputLength >= sizeof(Versions))
            RtlCopyMemory(&Versions, InputBuffer, sizeof(Versions));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;
    if (Versions.MinMajorVersion > Versions.MaxMajorVersion)
        return STATUS_INVALID_PARAMETER;
    if (Versions.MinMajorVersion <= 2 && Versions.MaxMajorVersion >= 2)
        Version = 2;
    else if (Versions.MinMajorVersion <= 3 && Versions.MaxMajorVersion >= 3)
        Version = 3;
    else
        return STATUS_INVALID_PARAMETER;

    Record = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_NTFS);
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(NtfsGetMainResource(FileCB), TRUE);
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnDescribeFile(VolCB, FileCB->FileRec, &Desc);
    if (NT_SUCCESS(Status))
    {
        HeaderLength = (Version == 3) ? FIELD_OFFSET(USN_RECORD_V3, FileName)
                                      : FIELD_OFFSET(USN_RECORD_V2, FileName);
        Length = ALIGN_UP_BY(HeaderLength + Desc.Name.Length, 8);
        if (Length > PAGE_SIZE)
            Status = STATUS_FILE_CORRUPT_ERROR;
    }
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Record, Length);
        if (Version == 3)
        {
            PUSN_RECORD_V3 R3 = (PUSN_RECORD_V3)Record;

            R3->RecordLength = Length;
            R3->MajorVersion = 3;
            RtlCopyMemory(&R3->FileReferenceNumber, &Desc.FileReference, sizeof(ULONGLONG));
            RtlCopyMemory(&R3->ParentFileReferenceNumber, &Desc.ParentReference, sizeof(ULONGLONG));
            R3->Usn = Desc.StoredUsn;
            R3->TimeStamp.QuadPart = Desc.ChangeTime;
            R3->SecurityId = Desc.SecurityId;
            R3->FileAttributes = Desc.FileAttributes;
            R3->FileNameLength = Desc.Name.Length;
            R3->FileNameOffset = (USHORT)HeaderLength;
        }
        else
        {
            PUSN_RECORD_V2 R2 = (PUSN_RECORD_V2)Record;

            R2->RecordLength = Length;
            R2->MajorVersion = 2;
            R2->FileReferenceNumber = Desc.FileReference;
            R2->ParentFileReferenceNumber = Desc.ParentReference;
            R2->Usn = Desc.StoredUsn;
            R2->TimeStamp.QuadPart = Desc.ChangeTime;
            R2->SecurityId = Desc.SecurityId;
            R2->FileAttributes = Desc.FileAttributes;
            R2->FileNameLength = Desc.Name.Length;
            R2->FileNameOffset = (USHORT)HeaderLength;
        }
        if (Desc.Name.Length)
            RtlCopyMemory(Record + HeaderLength, Desc.Name.Buffer, Desc.Name.Length);
    }
    NtfsReleaseMetadata(VolCB);
    ExReleaseResourceLite(NtfsGetMainResource(FileCB));
    KeLeaveCriticalRegion();

    if (NT_SUCCESS(Status) && OutputLength < Length)
        Status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(Status))
    {
        _SEH2_TRY
        {
            RtlCopyMemory(Output, Record, Length);
            Irp->IoStatus.Information = Length;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    ExFreePoolWithTag(Record, TAG_NTFS);
    return Status;
}

#define NTFS_USN_VALID_SOURCES (USN_SOURCE_DATA_MANAGEMENT | USN_SOURCE_AUXILIARY_DATA | \
                                USN_SOURCE_REPLICATION_MANAGEMENT | USN_SOURCE_CLIENT_REPLICATION_MANAGEMENT)

/*
 * Sets the USN source flags of this handle. The caller proves its rights over the volume by
 * passing a volume handle of the same volume opened for data access. Handle flags (cluster
 * protection, TxF log handles) are not supported and are refused rather than ignored.
 */
static
NTSTATUS
NtfsUsnMarkHandle(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    ULONG InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB, VolumeFileCB;
    PFILE_OBJECT VolumeFileObject;
    HANDLE VolumeHandle;
    ULONG SourceInfo, HandleInfo;
    NTSTATUS Status;

    Status = NtfsUsnGetFile(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!Buffer)
        return STATUS_INVALID_PARAMETER;
#ifdef _WIN64
    if (IoIs32bitProcess(Irp))
    {
        PMARK_HANDLE_INFO32 Info32 = (PMARK_HANDLE_INFO32)Buffer;

        if (InputLength < sizeof(MARK_HANDLE_INFO32))
            return STATUS_INVALID_PARAMETER;
        SourceInfo = Info32->UsnSourceInfo;
        VolumeHandle = (HANDLE)(ULONG_PTR)Info32->VolumeHandle;
        HandleInfo = Info32->HandleInfo;
    }
    else
#endif
    {
        PMARK_HANDLE_INFO Info = (PMARK_HANDLE_INFO)Buffer;

        if (InputLength < sizeof(MARK_HANDLE_INFO))
            return STATUS_INVALID_PARAMETER;
        SourceInfo = Info->UsnSourceInfo;
        VolumeHandle = Info->VolumeHandle;
        HandleInfo = Info->HandleInfo;
    }
    if ((SourceInfo & ~NTFS_USN_VALID_SOURCES) || HandleInfo != 0)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByHandle(VolumeHandle,
                                       0,
                                       *IoFileObjectType,
                                       Irp->RequestorMode,
                                       (PVOID*)&VolumeFileObject,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    /* The context of a file object is read only for a file object of this mounted volume */
    if (!VolumeFileObject->Vpb ||
        VolumeFileObject->Vpb != IrpSp->FileObject->Vpb ||
        VolumeFileObject->DeviceObject != IrpSp->FileObject->DeviceObject)
    {
        Status = STATUS_ACCESS_DENIED;
    }
    else
    {
        VolumeFileCB = NtfsGetFileContext(VolumeFileObject);
        if (!VolumeFileCB || !VolumeFileCB->IsVolumeOpen ||
            !(VolumeFileCB->DesiredAccess & (FILE_READ_DATA | FILE_WRITE_DATA)))
        {
            Status = STATUS_ACCESS_DENIED;
        }
    }
    ObDereferenceObject(VolumeFileObject);
    if (!NT_SUCCESS(Status))
        return Status;

    FileCB->UsnSourceInfo = SourceInfo;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsUsnEnum(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    MFT_ENUM_DATA_V1 Input;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    PVOID InputBuffer = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    ULONG InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PUCHAR Output, Staging = NULL;
    ULONG CopyLength, Capacity, Returned = 0;
    ULONGLONG Next = 0;
    BOOLEAN IsV1;
    NTSTATUS Status;

    Status = NtfsUsnGetVolume(VolumeDeviceObject, IrpSp, &VolCB, &FileCB);
    if (!NT_SUCCESS(Status))
        return Status;
    Output = GetUserBuffer(Irp);
    if (!InputBuffer || InputLength < sizeof(MFT_ENUM_DATA_V0))
        return STATUS_INVALID_PARAMETER;
    if (!Output || OutputLength < sizeof(ULONGLONG))
        return STATUS_BUFFER_TOO_SMALL;

    /* V1 adds the version range at the end of the V0 layout */
    IsV1 = InputLength >= FIELD_OFFSET(MFT_ENUM_DATA_V1, MaxMajorVersion) + sizeof(USHORT);
    CopyLength = IsV1 ? min(InputLength, (ULONG)sizeof(Input)) : sizeof(MFT_ENUM_DATA_V0);
    RtlZeroMemory(&Input, sizeof(Input));
    _SEH2_TRY
    {
        if (Irp->RequestorMode != KernelMode)
        {
            ProbeForRead(InputBuffer, CopyLength, sizeof(UCHAR));
            ProbeForWrite(Output, OutputLength, sizeof(UCHAR));
        }
        RtlCopyMemory(&Input, InputBuffer, CopyLength);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;
    if (!IsV1)
    {
        Input.MinMajorVersion = 2;
        Input.MaxMajorVersion = 2;
    }
    /* Only version 2 records are produced */
    if (Input.MinMajorVersion > 2 || Input.MaxMajorVersion < 2 || Input.LowUsn > Input.HighUsn)
        return STATUS_INVALID_PARAMETER;

    Capacity = min(OutputLength - (ULONG)sizeof(ULONGLONG), (ULONG)NTFS_USN_STAGING_MAX);
    if (Capacity == 0)
        return STATUS_BUFFER_TOO_SMALL;
    Staging = ExAllocatePoolWithTag(PagedPool, Capacity, TAG_NTFS);
    if (!Staging)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnJournalEnumerate(VolCB->DiskVolume,
                                     Input.StartFileReferenceNumber,
                                     Input.LowUsn,
                                     Input.HighUsn,
                                     Staging,
                                     Capacity,
                                     &Returned,
                                     &Next);
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();

    if (NT_SUCCESS(Status))
    {
        _SEH2_TRY
        {
            RtlCopyMemory(Output, &Next, sizeof(Next));
            RtlCopyMemory(Output + sizeof(ULONGLONG), Staging, Returned);
            Irp->IoStatus.Information = sizeof(ULONGLONG) + Returned;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }
    ExFreePoolWithTag(Staging, TAG_NTFS);
    return Status;
}

NTSTATUS
NtfsUsnFsControl(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    Irp->IoStatus.Information = 0;
    switch (IrpSp->Parameters.FileSystemControl.FsControlCode)
    {
        case FSCTL_CREATE_USN_JOURNAL:
            return NtfsUsnCreate(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_QUERY_USN_JOURNAL:
            return NtfsUsnQuery(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_READ_USN_JOURNAL:
            return NtfsUsnRead(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_WRITE_USN_CLOSE_RECORD:
            return NtfsUsnWriteCloseRecord(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_READ_FILE_USN_DATA:
            return NtfsUsnReadFileData(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_MARK_HANDLE:
            return NtfsUsnMarkHandle(VolumeDeviceObject, Irp, IrpSp);
        case FSCTL_ENUM_USN_DATA:
            return NtfsUsnEnum(VolumeDeviceObject, Irp, IrpSp);
        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}
