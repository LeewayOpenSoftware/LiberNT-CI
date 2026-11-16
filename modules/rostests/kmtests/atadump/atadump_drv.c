/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ATAPI crash-dump storage interface test, kernel half
 * COPYRIGHT:   Copyright 2026 Leeway Development Works
 *
 * Exercises the ROS crash-dump storage interface (IOCTL_REACTOS_STORAGE_
 * GET_DUMP_INTERFACE) end to end: Prepare, raw Write of a pattern through
 * the dump callbacks, Flush, read-back over the raw disk, and restore.
 *
 * The polled dump path bypasses all port serialization, so it is only safe
 * against a disk with no other I/O (at bugcheck the whole machine is
 * frozen). This test therefore drives ONLY a dedicated raw second disk
 * (e.g. a QEMU scratch image on its own IDE channel) and skips when none
 * is present. Never run it against the boot channel.
 */

#include <kmt_test.h>
#include <ntifs.h>
#include <ntdddisk.h>
#include <ntddvol.h>
#include <ntstrsafe.h>
#include <scsi.h>
#include <reactos/drivers/dumpstor.h>
#include <reactos/drivers/dumpscsi.h>

#define ATADUMP_TAG 'pmdA'
#define ATADUMP_VOLUME_PATH L"\\DosDevices\\C:"
#define ATADUMP_ALIGNMENT 512
#define ATADUMP_TOTAL_SECTORS 16
#define ATADUMP_MAX_DISKS 8
#define ATADUMP_LAYOUT_BUFFER_SIZE 4096

typedef struct _ATADUMP_BUFFERS
{
    PVOID Base[3];
    PUCHAR Save;
    PUCHAR Pattern;
    PUCHAR Read;
} ATADUMP_BUFFERS, *PATADUMP_BUFFERS;

static VOID
AtadumpFreeBuffers(
    _Inout_ PATADUMP_BUFFERS Buffers)
{
    INT i;

    for (i = 0; i < 3; i++)
    {
        if (Buffers->Base[i] != NULL)
            ExFreePoolWithTag(Buffers->Base[i], ATADUMP_TAG);
        Buffers->Base[i] = NULL;
    }
}

static BOOLEAN
AtadumpAllocateBuffers(
    _Inout_ PATADUMP_BUFFERS Buffers,
    _In_ ULONG Length)
{
    INT i;

    for (i = 0; i < 3; i++)
    {
        Buffers->Base[i] = ExAllocatePoolWithTag(NonPagedPool,
                                                 Length + ATADUMP_ALIGNMENT,
                                                 ATADUMP_TAG);
        if (Buffers->Base[i] == NULL)
        {
            AtadumpFreeBuffers(Buffers);
            return FALSE;
        }
    }

    Buffers->Save = (PUCHAR)(((ULONG_PTR)Buffers->Base[0] + ATADUMP_ALIGNMENT - 1) & ~((ULONG_PTR)ATADUMP_ALIGNMENT - 1));
    Buffers->Pattern = (PUCHAR)(((ULONG_PTR)Buffers->Base[1] + ATADUMP_ALIGNMENT - 1) & ~((ULONG_PTR)ATADUMP_ALIGNMENT - 1));
    Buffers->Read = (PUCHAR)(((ULONG_PTR)Buffers->Base[2] + ATADUMP_ALIGNMENT - 1) & ~((ULONG_PTR)ATADUMP_ALIGNMENT - 1));
    return TRUE;
}

static NTSTATUS
AtadumpOpenDevice(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG ShareAccess,
    _In_ ULONG OpenOptions)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwOpenFile(Handle, DesiredAccess, &ObjectAttributes, &IoStatus, ShareAccess, OpenOptions);
}

static NTSTATUS
AtadumpDumpWriteChunks(
    _In_ PROS_STORAGE_DUMP_INTERFACE DumpInterface,
    _In_ ULONGLONG DiskOffset,
    _In_ PUCHAR Buffer,
    _In_ ULONG Length)
{
    ULONG Chunk, Done;
    PHYSICAL_ADDRESS BufferPhysical;
    NTSTATUS Status = STATUS_SUCCESS;

    for (Done = 0; Done < Length; Done += Chunk)
    {
        Chunk = min(PAGE_SIZE, Length - Done);
        BufferPhysical = MmGetPhysicalAddress(Buffer + Done);
        Status = DumpInterface->WriteRoutine(DumpInterface->Context,
                                             DiskOffset + Done,
                                             BufferPhysical,
                                             Chunk);
        if (!NT_SUCCESS(Status))
            break;
    }
    return Status;
}

/* Find the boot disk number so the test never touches that channel. */
static BOOLEAN
AtadumpGetBootDiskNumber(
    _Out_ PULONG BootDiskNumber)
{
    HANDLE VolumeHandle = NULL;
    VOLUME_DISK_EXTENTS VolumeExtents;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    Status = AtadumpOpenDevice(&VolumeHandle,
                               ATADUMP_VOLUME_PATH,
                               FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = ZwDeviceIoControlFile(VolumeHandle, NULL, NULL, NULL, &IoStatus,
                                   IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                   NULL, 0, &VolumeExtents, sizeof(VolumeExtents));
    if (NT_SUCCESS(Status) && VolumeExtents.NumberOfDiskExtents == 1)
    {
        *BootDiskNumber = VolumeExtents.Extents[0].DiskNumber;
        Found = TRUE;
    }

    ZwClose(VolumeHandle);
    return Found;
}

/* Open a raw disk that carries no partitions (a dedicated scratch image). */
static NTSTATUS
AtadumpOpenDedicatedDisk(
    _Out_ PHANDLE DiskHandle,
    _Out_ PULONG SectorSize,
    _In_ ULONG BootDiskNumber)
{
    WCHAR DiskPath[64];
    UCHAR LayoutBuffer[ATADUMP_LAYOUT_BUFFER_SIZE];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PDRIVE_LAYOUT_INFORMATION_EX)LayoutBuffer;
    DISK_GEOMETRY_EX Geometry;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle = NULL;
    ULONG DiskNumber;
    NTSTATUS Status;

    for (DiskNumber = 0; DiskNumber < ATADUMP_MAX_DISKS; DiskNumber++)
    {
        if (DiskNumber == BootDiskNumber)
            continue;

        RtlStringCchPrintfW(DiskPath, RTL_NUMBER_OF(DiskPath),
                            L"\\Device\\Harddisk%lu\\Partition0", DiskNumber);
        Status = AtadumpOpenDevice(&Handle,
                                   DiskPath,
                                   FILE_READ_DATA | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   FILE_NO_INTERMEDIATE_BUFFERING);
        if (!NT_SUCCESS(Status))
            continue;

        /* Dedicated means no partition table: no volume can be mounted. */
        RtlZeroMemory(LayoutBuffer, sizeof(LayoutBuffer));
        Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                                       IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                                       NULL, 0, Layout, sizeof(LayoutBuffer));
        if (NT_SUCCESS(Status) && Layout->PartitionCount > 0)
        {
            ZwClose(Handle);
            Handle = NULL;
            continue;
        }

        Status = ZwDeviceIoControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                                       IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                       NULL, 0, &Geometry, sizeof(Geometry));
        if (!NT_SUCCESS(Status) || !RosDumpIsValidSectorSize(Geometry.Geometry.BytesPerSector))
        {
            ZwClose(Handle);
            Handle = NULL;
            continue;
        }

        *SectorSize = Geometry.Geometry.BytesPerSector;
        *DiskHandle = Handle;
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

static VOID
RunAtaDumpTest(VOID)
{
    ATADUMP_BUFFERS Buffers = { { NULL } };
    HANDLE DiskHandle = NULL;
    ROS_STORAGE_DUMP_INTERFACE DumpInterface;
    LARGE_INTEGER Offset;
    IO_STATUS_BLOCK IoStatus;
    ULONG SectorSize, BootDiskNumber = 0, Length, Done;
    NTSTATUS Status;
    BOOLEAN Saved = FALSE, Restored = FALSE;

    if (!AtadumpGetBootDiskNumber(&BootDiskNumber))
    {
        skip(TRUE, "Cannot identify the boot disk; refusing to guess which channel is live\n");
        goto Cleanup;
    }

    Status = AtadumpOpenDedicatedDisk(&DiskHandle, &SectorSize, BootDiskNumber);
    if (!NT_SUCCESS(Status))
    {
        skip(TRUE, "Needs a dedicated raw second disk with no partitions (e.g. -drive if=ide,index=2,scratch.raw); none found\n");
        goto Cleanup;
    }

    RtlZeroMemory(&DumpInterface, sizeof(DumpInterface));
    DumpInterface.BytesPerSector = SectorSize;
    Status = ZwDeviceIoControlFile(DiskHandle, NULL, NULL, NULL, &IoStatus,
                                   IOCTL_REACTOS_STORAGE_GET_DUMP_INTERFACE,
                                   &DumpInterface, sizeof(DumpInterface),
                                   &DumpInterface, sizeof(DumpInterface));
    if ((Status == STATUS_NOT_SUPPORTED) || (Status == STATUS_INVALID_DEVICE_REQUEST))
    {
        skip(TRUE, "Dump interface not supported (0x%lx): unpatched image, AHCI channel, or CHS volume\n", Status);
        goto Cleanup;
    }
    ok(NT_SUCCESS(Status), "IOCTL_REACTOS_STORAGE_GET_DUMP_INTERFACE failed: 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok(DumpInterface.Version == ROS_STORAGE_DUMP_INTERFACE_VERSION,
       "Version is %lu, expected %lu\n", DumpInterface.Version, ROS_STORAGE_DUMP_INTERFACE_VERSION);
    ok(DumpInterface.Prepare != NULL, "Prepare is NULL\n");
    ok(DumpInterface.WriteRoutine != NULL, "WriteRoutine is NULL\n");
    ok(DumpInterface.Flush != NULL, "Flush is NULL\n");
    ok(DumpInterface.BytesPerSector == SectorSize,
       "BytesPerSector is %lu, expected %lu\n", DumpInterface.BytesPerSector, SectorSize);
    ok(DumpInterface.MaximumTransferLength >= SectorSize &&
       (DumpInterface.MaximumTransferLength % SectorSize) == 0,
       "MaximumTransferLength %lu is not a sector multiple >= %lu\n",
       DumpInterface.MaximumTransferLength, SectorSize);
    if (!DumpInterface.Prepare || !DumpInterface.WriteRoutine || !DumpInterface.Flush)
        goto Cleanup;

    Length = ATADUMP_TOTAL_SECTORS * SectorSize;
    if (!AtadumpAllocateBuffers(&Buffers, Length))
    {
        ok(0, "Test buffer allocation failed\n");
        goto Cleanup;
    }

    /* Per-byte indexed pattern so partial writes cannot compare equal. */
    for (Done = 0; Done < Length; Done++)
        Buffers.Pattern[Done] = (UCHAR)(Done * 31 ^ 0x5A);

    /* Save the target sectors, then drive the polled path over them. */
    Offset.QuadPart = 0;
    Status = ZwReadFile(DiskHandle, NULL, NULL, NULL, &IoStatus, Buffers.Save, Length, &Offset, NULL);
    ok(NT_SUCCESS(Status), "Original-sector save failed: 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Saved = TRUE;

    Status = DumpInterface.Prepare(DumpInterface.Context);
    ok(NT_SUCCESS(Status), "Prepare failed: 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = AtadumpDumpWriteChunks(&DumpInterface, 0, Buffers.Pattern, Length);
        ok(NT_SUCCESS(Status), "Write failed: 0x%lx\n", Status);
    }
    if (NT_SUCCESS(Status))
    {
        Status = DumpInterface.Flush(DumpInterface.Context);
        ok(NT_SUCCESS(Status), "Flush failed: 0x%lx\n", Status);
    }

    if (NT_SUCCESS(Status))
    {
        Offset.QuadPart = 0;
        Status = ZwReadFile(DiskHandle, NULL, NULL, NULL, &IoStatus, Buffers.Read, Length, &Offset, NULL);
        ok(NT_SUCCESS(Status) && (RtlCompareMemory(Buffers.Read, Buffers.Pattern, Length) == Length),
           "Read-back does not match the written pattern (status 0x%lx)\n", Status);
    }

    if (Saved)
    {
        NTSTATUS RestoreStatus;

        RestoreStatus = DumpInterface.Prepare(DumpInterface.Context);
        if (NT_SUCCESS(RestoreStatus))
            RestoreStatus = AtadumpDumpWriteChunks(&DumpInterface, 0, Buffers.Save, Length);
        if (NT_SUCCESS(RestoreStatus))
            RestoreStatus = DumpInterface.Flush(DumpInterface.Context);
        ok(NT_SUCCESS(RestoreStatus), "Restore write failed: 0x%lx\n", RestoreStatus);

        if (NT_SUCCESS(RestoreStatus))
        {
            Offset.QuadPart = 0;
            Status = ZwReadFile(DiskHandle, NULL, NULL, NULL, &IoStatus, Buffers.Read, Length, &Offset, NULL);
            ok(NT_SUCCESS(Status) && (RtlCompareMemory(Buffers.Read, Buffers.Save, Length) == Length),
               "Restore verification failed (status 0x%lx)\n", Status);
            if (NT_SUCCESS(Status))
                Restored = TRUE;
        }
    }

Cleanup:
    if (Saved && !Restored)
        trace("WARNING: original sectors were not restored on the dedicated disk\n");
    AtadumpFreeBuffers(&Buffers);
    if (DiskHandle != NULL)
        ZwClose(DiskHandle);
}

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
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"AtaDump";
    RunAtaDumpTest();
    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DriverObject);
}
