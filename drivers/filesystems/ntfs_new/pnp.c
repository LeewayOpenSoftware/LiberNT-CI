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

/*
 * The last file object of a dismounted volume is gone and the release ran:
 * free what the mount built and delete the volume device. Anything that
 * still refers to the volume keeps the device in place, as before this
 * change, and says so once in the log.
 */
static
VOID
NtfsDeleteDismountedVolume(
    _In_ PDEVICE_OBJECT VolumeDeviceObject,
    _In_ PVolumeContextBlock VolCB)
{
    LIST_ENTRY IdleBlocks;
    PLIST_ENTRY Entry;
    PNtfsDirectory DoomedDir = NULL;
    PNtfsFileRecord DoomedParent;
    PVPB Vpb;
    KIRQL OldIrql;
    ULONG InUse;
    BOOLEAN FreeVpb = FALSE;
    BOOLEAN VpbStays = FALSE;
    PCSTR Blocker = NULL;

    PAGED_CODE();

    ExAcquireFastMutex(&VolCB->StreamListMutex);
    if (!IsListEmpty(&VolCB->StreamList))
        Blocker = "open streams";
    ExReleaseFastMutex(&VolCB->StreamListMutex);
    if (Blocker == NULL && VolCB->TxfActiveCount != 0)
        Blocker = "active transactions";

    /* The caches belong to this volume alone; they go whatever follows */
    NtfsForgetAllMissingNames(VolCB);
    InUse = NtfsPurgeRecordCache(VolCB);
    if (Blocker == NULL && InUse != 0)
        Blocker = "cached records in use";

    InitializeListHead(&IdleBlocks);
    ExAcquireFastMutex(&VolCB->IdleFcbMutex);
    while (!IsListEmpty(&VolCB->IdleFcbList))
    {
        Entry = RemoveHeadList(&VolCB->IdleFcbList);
        VolCB->IdleFcbCount--;
        InsertTailList(&IdleBlocks, Entry);
    }
    ExReleaseFastMutex(&VolCB->IdleFcbMutex);
    while (!IsListEmpty(&IdleBlocks))
    {
        PFileContextBlock FileCB = CONTAINING_RECORD(RemoveHeadList(&IdleBlocks), FileContextBlock, IdleLink);

        ExDeleteResourceLite(&FileCB->MainResource);
        ExDeleteResourceLite(&FileCB->PagingIoResource);
        ExFreePool(FileCB);
    }

    ExAcquireFastMutex(&VolCB->DirCacheMutex);
    if (VolCB->CachedDir != NULL)
    {
        if (VolCB->CachedDirBusy)
            Blocker = Blocker ? Blocker : "borrowed directory";
        else
        {
            DoomedDir = VolCB->CachedDir;
            VolCB->CachedDir = NULL;
        }
    }
    ExReleaseFastMutex(&VolCB->DirCacheMutex);
    if (DoomedDir)
        NtfsDirectoryDestroy(DoomedDir);

    ExAcquireFastMutex(&VolCB->CachedLookupParentMutex);
    DoomedParent = VolCB->CachedLookupParent;
    VolCB->CachedLookupParent = NULL;
    ExReleaseFastMutex(&VolCB->CachedLookupParentMutex);
    if (DoomedParent)
        NtfsFileRecordDestroy(DoomedParent);

    /* A dismount swapped a spare VPB onto the real device and made this
     * one persistent, so the I/O manager never frees it: that is this
     * driver's job, once nothing refers to it.
     * A PnP remove of a volume that was never dismounted swapped nothing:
     * the VPB is still the one of the real device and it stays with it. It
     * is only taken away from this volume device, in the same hold of the
     * lock that found it unreferenced, so that no mount comes in between. */
    Vpb = VolCB->Vpb;
    IoAcquireVpbSpinLock(&OldIrql);
    if (Vpb != NULL && Vpb->ReferenceCount == 0 && Vpb->RealDevice != NULL)
    {
        if (Vpb->RealDevice->Vpb != Vpb)
        {
            if (Vpb->Flags & VPB_PERSISTENT)
                FreeVpb = TRUE;
        }
        else if (Blocker == NULL)
        {
            if (Vpb->DeviceObject == VolumeDeviceObject)
            {
                Vpb->DeviceObject = NULL;
                Vpb->Flags &= ~(VPB_MOUNTED | VPB_LOCKED | VPB_DIRECT_WRITES_ALLOWED);
            }
            VpbStays = TRUE;
        }
    }
    IoReleaseVpbSpinLock(OldIrql);
    if (Blocker == NULL && Vpb != NULL && !FreeVpb && !VpbStays)
        Blocker = "referenced VPB";

    if (Blocker != NULL)
    {
        DPRINT1("NTFS: dismounted volume %p kept (%s)\n", VolumeDeviceObject, Blocker);
        return;
    }

    /* The dismount path unregistered already; the PnP remove path did not */
    IoUnregisterShutdownNotification(VolumeDeviceObject);

    FsRtlNotifyUninitializeSync(&VolCB->NotifySync);
    FsRtlDeleteTunnelCache(&VolCB->Tunnel);
    NtfsTxfReleaseVolumeName(VolCB);

    NtfsVolumeDestroy(VolCB->DiskVolume);
    VolCB->DiskVolume = NULL;
    ExDeleteResourceLite(&VolCB->MetadataResource);

    if (Vpb != NULL)
    {
        IoAcquireVpbSpinLock(&OldIrql);
        if (FreeVpb)
            Vpb->DeviceObject = NULL;
        if (VolumeDeviceObject->Vpb == Vpb)
            VolumeDeviceObject->Vpb = NULL;
        IoReleaseVpbSpinLock(OldIrql);
        VolCB->Vpb = NULL;
        if (FreeVpb)
            ExFreePool(Vpb);
        else
            DPRINT1("NTFS: removed volume %p was never dismounted, its VPB stays with the real device\n",
                    VolumeDeviceObject);
    }

    DPRINT1("NTFS: dismounted volume %p released after its last file object, device deleted\n",
            VolumeDeviceObject);
    IoDeleteDevice(VolumeDeviceObject);
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
    if (VolCB->Dismounted && VolCB->FileObjectCount == 0 && !VolCB->BootRepairDismount)
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
         * retried by the lazy writer, fail-closed). */
        NtfsDiskReleaseKm(VolCB->StorageDevice);
        ObDereferenceObject(StreamFileObject);
        NtfsDeleteDismountedVolume(VolumeDeviceObject, VolCB);
    }
}

typedef struct _NTFS_RELEASE_WORK
{
    WORK_QUEUE_ITEM Item;
    PDEVICE_OBJECT VolumeDeviceObject;
    PDEVICE_OBJECT RealDevice;
} NTFS_RELEASE_WORK, *PNTFS_RELEASE_WORK;

static
VOID
NTAPI
NtfsReleaseDismountedVolumeWorker(_In_ PVOID Context)
{
    PNTFS_RELEASE_WORK Work = Context;
    PDEVICE_OBJECT VolumeDeviceObject = Work->VolumeDeviceObject;
    PDEVICE_OBJECT RealDevice = Work->RealDevice;

    ExFreePoolWithTag(Work, TAG_NTFS);
    NtfsReleaseDismountedVolume(VolumeDeviceObject);
    ObDereferenceObject(VolumeDeviceObject);
    if (RealDevice)
        ObDereferenceObject(RealDevice);
}

/* The last file object closed on a dismounted volume. IRP_MJ_CLOSE may arrive
 * above PASSIVE_LEVEL (a deferred dereference from Cc or Mm); the release
 * flushes and waits, so it runs on a worker then. */
VOID
NtfsReleaseDismountedVolumeFromClose(_In_ PDEVICE_OBJECT VolumeDeviceObject)
{
    PVolumeContextBlock VolCB = (PVolumeContextBlock)VolumeDeviceObject->DeviceExtension;
    PNTFS_RELEASE_WORK Work;

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        NtfsReleaseDismountedVolume(VolumeDeviceObject);
        return;
    }

    /* Out of pool: the volume stays as the dismount left it (no data at
     * risk, everything was flushed on dismount); a PnP remove still releases it. */
    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), TAG_NTFS);
    if (Work == NULL)
    {
        DPRINT1("NTFS: no pool to release dismounted volume %p; it stays until a PnP remove\n",
                VolumeDeviceObject);
        return;
    }
    ObReferenceObject(VolumeDeviceObject);
    Work->VolumeDeviceObject = VolumeDeviceObject;
    /* The release reads the real device through the VPB. The file object
     * that is being closed keeps that device only until this close returns. */
    Work->RealDevice = VolCB->Vpb ? VolCB->Vpb->RealDevice : NULL;
    if (Work->RealDevice)
        ObReferenceObject(Work->RealDevice);
    ExInitializeWorkItem(&Work->Item, NtfsReleaseDismountedVolumeWorker, Work);
    ExQueueWorkItem(&Work->Item, DelayedWorkQueue);
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
