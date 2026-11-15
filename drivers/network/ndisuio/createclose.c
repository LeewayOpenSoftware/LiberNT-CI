/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        createclose.c
 * PURPOSE:     IRP_MJ_CREATE and IRP_MJ_CLOSE handling
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

//#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
NduDispatchCreate(PDEVICE_OBJECT DeviceObject,
                  PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    ASSERT(DeviceObject == GlobalDeviceObject);

    DPRINT("Created file object 0x%x\n", IrpSp->FileObject);

    /* Reject requests with trailing names - ndisuio has no namespace */
    if (IrpSp->FileObject->FileName.Length > 0)
    {
        DPRINT1("Rejected create with trailing name\n");
        Irp->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* This is associated with an adapter during IOCTL_NDISUIO_OPEN_(WRITE_)DEVICE */
    IrpSp->FileObject->FsContext = NULL;
    IrpSp->FileObject->FsContext2 = NULL;

    /* Completed successfully */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = FILE_OPENED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    /* Return success */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NduDispatchClose(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNDISUIO_OPEN_ENTRY OpenEntry = NULL;
    LIST_ENTRY StalePackets;
    PLIST_ENTRY CurrentEntry;
    KIRQL OldIrql;

    ASSERT(DeviceObject == GlobalDeviceObject);

    InitializeListHead(&StalePackets);

    DPRINT("Closing file object 0x%x\n", FileObject);

    /* Check if this handle is associated with an adapter. The unbind clears
     * the association under the same lock, so it is either whole or gone. */
    KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
    AdapterContext = FileObject->FsContext;
    if (AdapterContext != NULL)
    {
        OpenEntry = FileObject->FsContext2;
        ASSERT(OpenEntry != NULL);

        KeAcquireSpinLockAtDpcLevel(&AdapterContext->Spinlock);
        RemoveEntryList(&OpenEntry->ListEntry);
        AdapterContext->OpenCount--;

        /* The packets that waited for this handle are of no use to the next */
        if (AdapterContext->OpenCount == 0)
        {
            while (!IsListEmpty(&AdapterContext->PacketList))
            {
                CurrentEntry = RemoveHeadList(&AdapterContext->PacketList);
                InsertTailList(&StalePackets, CurrentEntry);
            }
            AdapterContext->PacketCount = 0;
        }
        KeReleaseSpinLockFromDpcLevel(&AdapterContext->Spinlock);

        FileObject->FsContext = NULL;
        FileObject->FsContext2 = NULL;
    }
    KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);

    if (OpenEntry != NULL)
        ExFreePool(OpenEntry);

    while (!IsListEmpty(&StalePackets))
    {
        CurrentEntry = RemoveHeadList(&StalePackets);
        ExFreePool(CONTAINING_RECORD(CurrentEntry, NDISUIO_PACKET_ENTRY, ListEntry));
    }

    /* Completed */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    /* Return success */
    return STATUS_SUCCESS;
}
