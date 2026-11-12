/*
 * PROJECT:     LiberNT test drivers
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     flushfail: a volume filter that presents the volume below it
 *              as a disk with a volatile write cache that rejects cache
 *              flushes and does not support FUA (TESTPLAN-DEVICE-WRITE-CACHE-
 *              POWERLOSS.md, N3). Test-only: loaded by flushfail.exe, attached
 *              on request to one volume device, never unloaded.
 *
 *              What it changes for the file system mounted through it:
 *              - IRP_MJ_FLUSH_BUFFERS completes with STATUS_INVALID_DEVICE_REQUEST
 *              - in writefail mode, IRP_MJ_WRITE completes with STATUS_DEVICE_DATA_ERROR
 *              - IOCTL_STORAGE_QUERY_PROPERTY(StorageDeviceWriteCacheProperty)
 *                answers write cache enabled, write-back, FUA not supported,
 *                flush not supported
 *              - IOCTL_DISK_GET_CACHE_INFORMATION answers write cache enabled
 *              Everything else passes through unchanged; writes are counted.
 */

#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include "flushfail.h"

typedef enum _FLUSHFAIL_DEVICE_KIND
{
    FlushFailControl,
    FlushFailFilter
} FLUSHFAIL_DEVICE_KIND;

typedef struct _FLUSHFAIL_EXTENSION
{
    FLUSHFAIL_DEVICE_KIND Kind;
    PDEVICE_OBJECT Lower;   /* filter only: the device we attached to */
} FLUSHFAIL_EXTENSION, *PFLUSHFAIL_EXTENSION;

static PDEVICE_OBJECT FlushFailControlDevice;
static FLUSHFAIL_STATS FlushFailStats;
/* writefail mode: writes below the filtered volume fail (IOCTL_FLUSHFAIL_WRITEFAIL). */
static volatile LONG FlushFailWriteFail;

static
NTSTATUS
FlushFailComplete(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
FlushFailPassThrough(_In_ PFLUSHFAIL_EXTENSION Ext, _In_ PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Ext->Lower, Irp);
}

/* Rewrites the answer of the two cache queries after the device below
 * filled it in; the real answer's other fields are kept. */
static
NTSTATUS
NTAPI
FlushFailCacheQueryComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    ULONG Code = (ULONG)(ULONG_PTR)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
    if (NT_SUCCESS(Irp->IoStatus.Status) && Irp->AssociatedIrp.SystemBuffer)
    {
        if (Code == IOCTL_STORAGE_QUERY_PROPERTY &&
            Irp->IoStatus.Information >= sizeof(STORAGE_WRITE_CACHE_PROPERTY))
        {
            PSTORAGE_WRITE_CACHE_PROPERTY P = Irp->AssociatedIrp.SystemBuffer;
            P->WriteCacheEnabled = WriteCacheEnabled;
            P->WriteCacheType = WriteCacheTypeWriteBack;
            P->WriteCacheChangeable = WriteCacheNotChangeable;
            P->WriteThroughSupported = WriteThroughNotSupported;
            P->FlushCacheSupported = FALSE;
            P->UserDefinedPowerProtection = FALSE;
            P->NVCacheEnabled = FALSE;
            InterlockedIncrement((PLONG)&FlushFailStats.CacheQueriesFaked);
        }
        else if (Code == IOCTL_DISK_GET_CACHE_INFORMATION &&
                 Irp->IoStatus.Information >= sizeof(DISK_CACHE_INFORMATION))
        {
            PDISK_CACHE_INFORMATION C = Irp->AssociatedIrp.SystemBuffer;
            C->WriteCacheEnabled = TRUE;
            InterlockedIncrement((PLONG)&FlushFailStats.CacheQueriesFaked);
        }
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
FlushFailFilterDispatch(_In_ PFLUSHFAIL_EXTENSION Ext, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction)
    {
        case IRP_MJ_FLUSH_BUFFERS:
            /* The modelled device has no working flush: reject, as a bridge
             * that does not implement SYNCHRONIZE CACHE would. */
            InterlockedIncrement((PLONG)&FlushFailStats.FlushesRejected);
            return FlushFailComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

        case IRP_MJ_WRITE:
            InterlockedIncrement((PLONG)&FlushFailStats.WritesSeen);
            if (IrpSp->Flags & SL_WRITE_THROUGH)
                InterlockedIncrement((PLONG)&FlushFailStats.WriteThroughSeen);
            if (FlushFailWriteFail)
            {
                InterlockedIncrement((PLONG)&FlushFailStats.WritesRejected);
                return FlushFailComplete(Irp, STATUS_DEVICE_DATA_ERROR, 0);
            }
            return FlushFailPassThrough(Ext, Irp);

        case IRP_MJ_DEVICE_CONTROL:
        {
            ULONG Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;
            BOOLEAN Hook = FALSE;

            if (Code == IOCTL_STORAGE_QUERY_PROPERTY &&
                IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(STORAGE_PROPERTY_QUERY) &&
                Irp->AssociatedIrp.SystemBuffer)
            {
                PSTORAGE_PROPERTY_QUERY Q = Irp->AssociatedIrp.SystemBuffer;
                Hook = (Q->PropertyId == StorageDeviceWriteCacheProperty &&
                        Q->QueryType == PropertyStandardQuery);
            }
            else if (Code == IOCTL_DISK_GET_CACHE_INFORMATION)
            {
                Hook = TRUE;
            }
            if (!Hook)
                return FlushFailPassThrough(Ext, Irp);
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, FlushFailCacheQueryComplete, (PVOID)(ULONG_PTR)Code, TRUE, TRUE, TRUE);
            return IoCallDriver(Ext->Lower, Irp);
        }

        case IRP_MJ_POWER:
            PoStartNextPowerIrp(Irp);
            IoSkipCurrentIrpStackLocation(Irp);
            return PoCallDriver(Ext->Lower, Irp);

        case IRP_MJ_PNP:
            if (IrpSp->MinorFunction == IRP_MN_REMOVE_DEVICE)
            {
                PDEVICE_OBJECT Self = IrpSp->DeviceObject;
                NTSTATUS Status;

                IoSkipCurrentIrpStackLocation(Irp);
                Status = IoCallDriver(Ext->Lower, Irp);
                IoDetachDevice(Ext->Lower);
                IoDeleteDevice(Self);
                return Status;
            }
            return FlushFailPassThrough(Ext, Irp);

        default:
            return FlushFailPassThrough(Ext, Irp);
    }
}

static
NTSTATUS
FlushFailAttach(_In_ PDRIVER_OBJECT DriverObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG Length = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    PWCHAR Buffer = Irp->AssociatedIrp.SystemBuffer;
    UNICODE_STRING Name;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Target, Filter, Lower;
    PFLUSHFAIL_EXTENSION Ext;
    NTSTATUS Status;

    if (!Buffer || Length < 2 * sizeof(WCHAR) || (Length % sizeof(WCHAR)) != 0 ||
        Buffer[Length / sizeof(WCHAR) - 1] != UNICODE_NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (FlushFailStats.Attached)
        return STATUS_DEVICE_BUSY;
    RtlInitUnicodeString(&Name, Buffer);

    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_ATTRIBUTES, &FileObject, &Target);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoCreateDevice(DriverObject, sizeof(FLUSHFAIL_EXTENSION), NULL,
                            FILE_DEVICE_DISK, Target->Characteristics & FILE_DEVICE_SECURE_OPEN,
                            FALSE, &Filter);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(FileObject);
        return Status;
    }
    Ext = Filter->DeviceExtension;
    RtlZeroMemory(Ext, sizeof(*Ext));
    Ext->Kind = FlushFailFilter;

    Status = IoAttachDeviceToDeviceStackSafe(Filter, Target, &Lower);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(Filter);
        ObDereferenceObject(FileObject);
        return Status;
    }
    Ext->Lower = Lower;
    Filter->Flags |= Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    Filter->AlignmentRequirement = Lower->AlignmentRequirement;
    Filter->StackSize = Lower->StackSize + 1;
    Filter->Flags &= ~DO_DEVICE_INITIALIZING;
    /* The file object was only the way to the device; the attachment holds it. */
    ObDereferenceObject(FileObject);

    InterlockedExchange((PLONG)&FlushFailStats.Attached, 1);
    DbgPrint("flushfail: attached above %wZ (filter %p, lower %p)\n", &Name, Filter, Lower);
    return STATUS_SUCCESS;
}

static
NTSTATUS
FlushFailControlDispatch(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction)
    {
        case IRP_MJ_CREATE:
        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            return FlushFailComplete(Irp, STATUS_SUCCESS, 0);

        case IRP_MJ_DEVICE_CONTROL:
            switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
            {
                case IOCTL_FLUSHFAIL_ATTACH:
                    return FlushFailComplete(Irp, FlushFailAttach(DeviceObject->DriverObject, Irp), 0);

                case IOCTL_FLUSHFAIL_WRITEFAIL:
                    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG))
                        return FlushFailComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                    InterlockedExchange(&FlushFailWriteFail, *(PULONG)Irp->AssociatedIrp.SystemBuffer != 0);
                    DbgPrint("flushfail: writefail %s\n", FlushFailWriteFail ? "on: every write below the volume fails" : "off");
                    return FlushFailComplete(Irp, STATUS_SUCCESS, 0);

                case IOCTL_FLUSHFAIL_STATS:
                    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(FLUSHFAIL_STATS))
                        return FlushFailComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &FlushFailStats, sizeof(FLUSHFAIL_STATS));
                    return FlushFailComplete(Irp, STATUS_SUCCESS, sizeof(FLUSHFAIL_STATS));

                default:
                    return FlushFailComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
            }

        default:
            return FlushFailComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static
NTSTATUS
NTAPI
FlushFailDispatch(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PFLUSHFAIL_EXTENSION Ext = DeviceObject->DeviceExtension;

    if (Ext->Kind == FlushFailFilter)
        return FlushFailFilterDispatch(Ext, Irp);
    return FlushFailControlDispatch(DeviceObject, Irp);
}

NTSTATUS
NTAPI
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(FLUSHFAIL_DEVICE_NAME);
    UNICODE_STRING DosName = RTL_CONSTANT_STRING(FLUSHFAIL_DOS_NAME);
    PFLUSHFAIL_EXTENSION Ext;
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = FlushFailDispatch;
    /* No DriverUnload: a filter in a live stack is never unloaded. */

    Status = IoCreateDevice(DriverObject, sizeof(FLUSHFAIL_EXTENSION), &DeviceName,
                            FILE_DEVICE_FLUSHFAIL, FILE_DEVICE_SECURE_OPEN, FALSE,
                            &FlushFailControlDevice);
    if (!NT_SUCCESS(Status))
        return Status;
    Ext = FlushFailControlDevice->DeviceExtension;
    RtlZeroMemory(Ext, sizeof(*Ext));
    Ext->Kind = FlushFailControl;
    FlushFailControlDevice->Flags |= DO_BUFFERED_IO;
    FlushFailControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoCreateSymbolicLink(&DosName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(FlushFailControlDevice);
        return Status;
    }
    DbgPrint("flushfail: loaded, control device ready\n");
    return STATUS_SUCCESS;
}
