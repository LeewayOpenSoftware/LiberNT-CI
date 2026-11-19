/*
 * PROJECT:     LiberNT NTFS driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USN change journal FSCTLs: create, query and read
 */

#include "ntfspch.h"

/* Records are staged in pool and copied to the caller after the metadata lock is dropped */
#define NTFS_USN_STAGING_MAX    (64 * 1024)
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

static
NTSTATUS
NtfsUsnRead(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IrpSp)
{
    READ_USN_JOURNAL_DATA_V1 Input;
    NtfsUsnJournalData Data;
    PVolumeContextBlock VolCB;
    PFileContextBlock FileCB;
    PVOID InputBuffer = IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    ULONG InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    PUCHAR Output;
    PUCHAR Staging = NULL;
    PUCHAR Raw = NULL;
    ULONG CopyLength, Capacity, Filled = 0, RawLength, Offset;
    BOOLEAN IsV1;
    LONGLONG Cursor, Next;
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

    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    Status = NtfsUsnJournalQuery(VolCB->DiskVolume, &Data);
    if (NT_SUCCESS(Status) && Data.UsnJournalId != Input.UsnJournalID)
        Status = STATUS_INVALID_PARAMETER;

    /* Filtered-out records still advance the USN returned for the next read */
    Cursor = Input.StartUsn;
    while (NT_SUCCESS(Status) && Filled < Capacity)
    {
        Status = NtfsUsnJournalRead(VolCB->DiskVolume, Cursor, Raw, Capacity - Filled, &RawLength, &Next);
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

            if (NtfsUsnRecordSelected(Raw + Offset, &Input))
            {
                RtlCopyMemory(Staging + Filled, Raw + Offset, Header->RecordLength);
                Filled += Header->RecordLength;
            }
            Offset += Header->RecordLength;
        }
        Cursor = Next;
        if (RawLength == 0)
            break;
    }
    /* 0 asks for the first record; report the real USN reading continues from */
    if (NT_SUCCESS(Status) && Cursor == 0)
        Cursor = Data.FirstUsn;
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();
    if (!NT_SUCCESS(Status))
        goto Cleanup;

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
        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}
