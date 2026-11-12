/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Source file for the ntfs_new entry point
 * COPYRIGHT:   Copyright 2024 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2024 Carl J. Bialorucki <carl.bialorucki@reactos.org>
 */

#include "ntfspch.h"

/* GLOBALS *****************************************************************/

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, NtfsFsdPnp)
#endif

/* FUNCTIONS ****************************************************************/

static
VOID
NtfsUnlockVolumeForPnp(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVolumeContextBlock VolCB)
{
    KIRQL OldIrql;

    ExAcquireFastMutex(&VolCB->VolumeStateMutex);
    if (VolCB->VolumeLockOwner &&
        VolCB->VolumeLockOwner == VolCB->StreamFileObject)
    {
        VolCB->VolumeLockOwner = NULL;
        IoAcquireVpbSpinLock(&OldIrql);
        VolumeDeviceObject->Vpb->Flags &= ~VPB_LOCKED;
        IoReleaseVpbSpinLock(OldIrql);
    }
    ExReleaseFastMutex(&VolCB->VolumeStateMutex);
}

static
NTSTATUS
NtfsLockVolumeForPnp(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVolumeContextBlock VolCB)
{
    KIRQL OldIrql;
    NTSTATUS Status;

    ExAcquireFastMutex(&VolCB->VolumeStateMutex);
    if (VolCB->Dismounted)
        Status = STATUS_VOLUME_DISMOUNTED;
    else if (VolCB->Dismounting ||
             VolCB->VolumeLockOwner ||
             VolCB->OpenHandleCount != 0 ||
             !VolCB->StreamFileObject)
        Status = STATUS_ACCESS_DENIED;
    else
    {
        VolCB->VolumeLockOwner = VolCB->StreamFileObject;
        IoAcquireVpbSpinLock(&OldIrql);
        VolumeDeviceObject->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(OldIrql);
        Status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&VolCB->VolumeStateMutex);

    if (Status == STATUS_VOLUME_DISMOUNTED)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsFlushVolume(VolCB, TRUE);
    if (!NT_SUCCESS(Status))
        NtfsUnlockVolumeForPnp(VolumeDeviceObject, VolCB);
    return Status;
}

static
VOID
NtfsMarkVolumeRemoved(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVolumeContextBlock VolCB)
{
    KIRQL OldIrql;

    ExAcquireFastMutex(&VolCB->VolumeStateMutex);
    VolCB->Dismounted = TRUE;
    ExReleaseFastMutex(&VolCB->VolumeStateMutex);

    KeEnterCriticalRegion();
    NtfsAcquireMetadata(VolCB);
    NtfsReleaseMetadata(VolCB);
    KeLeaveCriticalRegion();

    IoAcquireVpbSpinLock(&OldIrql);
    VolumeDeviceObject->Vpb->Flags &= ~VPB_MOUNTED;
    IoReleaseVpbSpinLock(OldIrql);
}

VOID
NtfsReleaseDismountedVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject)
{
    PVolumeContextBlock VolCB =
        (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension;
    PFILE_OBJECT StreamFileObject = NULL;
    KIRQL OldIrql;

    ExAcquireFastMutex(&VolCB->VolumeStateMutex);
    if (VolCB->Dismounted && VolCB->OpenHandleCount == 0)
    {
        StreamFileObject = VolCB->StreamFileObject;
        VolCB->StreamFileObject = NULL;
        if (StreamFileObject && VolCB->VolumeLockOwner == StreamFileObject)
        {
            VolCB->VolumeLockOwner = NULL;
            IoAcquireVpbSpinLock(&OldIrql);
            VolumeDeviceObject->Vpb->Flags &= ~VPB_LOCKED;
            IoReleaseVpbSpinLock(OldIrql);
        }
    }
    ExReleaseFastMutex(&VolCB->VolumeStateMutex);

    if (StreamFileObject)
    {
        NtfsUnregisterVolumeForShutdown(VolCB);
        NtfsFlushVolume(VolCB, TRUE);
        /* The storage device object may be reused by another volume: its clean
         * cached blocks and what the environment learned about it for media
         * ordering go with us (dirty blocks, if the flush failed, stay and are
         * retried by the lazy writer, fail-closed). Not while the next volume
         * already runs on the same storage device: those blocks and that
         * answer are its own now, and its mount preparation started them
         * afresh. */
        if (!NtfsIsStorageDeviceInUse(VolCB->StorageDevice, VolCB))
            NtfsDiskReleaseKm(VolCB->StorageDevice);
        ObDereferenceObject(StreamFileObject);
    }
}

/*
 * Called by the close of a file object that carried a context once the
 * volume has none left. A volume that is still mounted keeps going; a
 * dismounted one can receive no further request (every file object of it is
 * closed, and the device's VPB already belongs to the next mount), so its
 * volume context and device are released here.
 */
VOID
NtfsDeleteDismountedVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject)
{
    PVolumeContextBlock VolCB =
        (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension;
    PFILE_OBJECT StreamFileObject;
    PVPB Vpb;
    PVPB FreeVpb = NULL;
    KIRQL OldIrql;

    if (!VolCB->Dismounted)
        return;

    ExAcquireFastMutex(&VolCB->VolumeStateMutex);
    if (!VolCB->Dismounted || VolCB->Deleted || VolCB->FileObjectCount != 0)
    {
        ExReleaseFastMutex(&VolCB->VolumeStateMutex);
        return;
    }
    VolCB->Deleted = TRUE;
    StreamFileObject = VolCB->StreamFileObject;
    VolCB->StreamFileObject = NULL;
    VolCB->VolumeLockOwner = NULL;
    ExReleaseFastMutex(&VolCB->VolumeStateMutex);

    NtfsUnregisterVolumeForShutdown(VolCB);
    if (StreamFileObject)
        ObDereferenceObject(StreamFileObject);

    DPRINT1("NTFS: deleting dismounted volume %p (storage %p)\n",
            VolumeDeviceObject, VolCB->StorageDevice);

    ExAcquireFastMutex(&VolCB->DirCacheMutex);
    if (VolCB->CachedDir && !VolCB->CachedDirBusy)
    {
        NtfsDirectoryDestroy(VolCB->CachedDir);
        VolCB->CachedDir = NULL;
    }
    ExReleaseFastMutex(&VolCB->DirCacheMutex);

    ExAcquireFastMutex(&VolCB->CachedLookupParentMutex);
    if (VolCB->CachedLookupParent)
    {
        NtfsFileRecordDestroy(VolCB->CachedLookupParent);
        VolCB->CachedLookupParent = NULL;
    }
    ExReleaseFastMutex(&VolCB->CachedLookupParentMutex);

    ExAcquireFastMutex(&VolCB->RecordCacheMutex);
    while (!IsListEmpty(&VolCB->RecordCacheList))
    {
        PNtfsCachedRecord Entry =
            CONTAINING_RECORD(VolCB->RecordCacheList.Flink, NtfsCachedRecord, Link);

        RemoveEntryList(&Entry->Link);
        RemoveEntryList(&Entry->HashLink);
        RemoveEntryList(&Entry->IdentityLink);
        VolCB->RecordCacheCount--;
        /* A record still in use (none should be) is leaked, not freed. */
        if (Entry->InUse == 0)
        {
            if (Entry->Record)
                NtfsFileRecordDestroy(Entry->Record);
            ExFreePoolWithTag(Entry, TAG_NTFS);
        }
    }
    ExReleaseFastMutex(&VolCB->RecordCacheMutex);

    NtfsForgetAllMissingNames(VolCB);

    ExAcquireFastMutex(&VolCB->IdleFcbMutex);
    while (!IsListEmpty(&VolCB->IdleFcbList))
    {
        PFileContextBlock FileCB =
            CONTAINING_RECORD(RemoveHeadList(&VolCB->IdleFcbList), FileContextBlock, IdleLink);

        VolCB->IdleFcbCount--;
        ExDeleteResourceLite(&FileCB->MainResource);
        ExDeleteResourceLite(&FileCB->PagingIoResource);
        ExFreePool(FileCB);
    }
    ExReleaseFastMutex(&VolCB->IdleFcbMutex);

    FsRtlNotifyUninitializeSync(&VolCB->NotifySync);
    FsRtlDeleteTunnelCache(&VolCB->Tunnel);

    if (VolCB->DiskVolume)
    {
        NtfsVolumeDestroy(VolCB->DiskVolume);
        VolCB->DiskVolume = NULL;
    }

    ExDeleteResourceLite(&VolCB->MetadataResource);

    /*
     * The I/O manager dropped the closing file object's VPB reference before
     * the close. The old VPB is no longer the device's (the dismount swapped
     * it) and was kept persistent for this volume, so it is freed here once
     * nothing references it.
     */
    IoAcquireVpbSpinLock(&OldIrql);
    Vpb = VolumeDeviceObject->Vpb;
    if (Vpb && Vpb->RealDevice && Vpb->RealDevice->Vpb != Vpb)
    {
        Vpb->DeviceObject = NULL;
        Vpb->Flags &= ~VPB_PERSISTENT;
        if (Vpb->ReferenceCount == 0)
            FreeVpb = Vpb;
    }
    VolumeDeviceObject->Vpb = NULL;
    IoReleaseVpbSpinLock(OldIrql);

    if (FreeVpb)
        ExFreePool(FreeVpb);

    IoDeleteDevice(VolumeDeviceObject);
}

_Function_class_(IRP_MJ_PNP)
_Function_class_(DRIVER_DISPATCH)
NTSTATUS
NTAPI
NtfsFsdPnp(_In_ PDEVICE_OBJECT VolumeDeviceObject,
           _Inout_ PIRP Irp)
{
    /* Overview:
     * Handles PnP requests.
     * See: https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/irp-mj-pnp
     */
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PVolumeContextBlock VolCB;
    NTSTATUS Status;

    PAGED_CODE();

    if (VolumeDeviceObject == NtfsDiskFileSystemDeviceObject)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    VolCB = (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension;
    NtfsBindVolumeDisk(VolCB);

    switch (IrpSp->MinorFunction)
    {
        case IRP_MN_QUERY_REMOVE_DEVICE:
            Status = NtfsLockVolumeForPnp(VolumeDeviceObject, VolCB);
            if (NT_SUCCESS(Status))
            {
                IoForwardIrpSynchronously(VolCB->StorageDevice, Irp);
                Status = Irp->IoStatus.Status;
                if (!NT_SUCCESS(Status))
                    NtfsUnlockVolumeForPnp(VolumeDeviceObject, VolCB);
            }
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE:
            IoForwardIrpSynchronously(VolCB->StorageDevice, Irp);
            Status = Irp->IoStatus.Status;
            NtfsUnlockVolumeForPnp(VolumeDeviceObject, VolCB);
            break;

        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            NtfsMarkVolumeRemoved(VolumeDeviceObject, VolCB);
            IoForwardIrpSynchronously(VolCB->StorageDevice, Irp);
            Status = Irp->IoStatus.Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            NtfsReleaseDismountedVolume(VolumeDeviceObject);
            return Status;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(VolCB->StorageDevice, Irp);
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
