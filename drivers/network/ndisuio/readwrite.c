/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        readwrite.c
 * PURPOSE:     Handles IRP_MJ_READ and IRP_MJ_WRITE
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

#define NDEBUG
#include <debug.h>

static
VOID
NTAPI
ReadIrpCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = Irp->Tail.Overlay.DriverContext[0];
    PNDISUIO_PACKET_ENTRY PacketEntry;

    /* The reader keeps the adapter alive, and it cannot remove this routine
     * and leave while the cancel spin lock is held. So the lock is released
     * last. The file object is not used: an unbind clears its context. */

    /* Indicate a 0-byte packet on the queue to cancel the read */
    PacketEntry = ExAllocatePool(NonPagedPool, sizeof(NDISUIO_PACKET_ENTRY));
    if (PacketEntry)
    {
        PacketEntry->PacketLength = 0;

        ExInterlockedInsertHeadList(&AdapterContext->PacketList,
                                    &PacketEntry->ListEntry,
                                    &AdapterContext->Spinlock);

        KeSetEvent(&AdapterContext->PacketReadEvent, IO_NO_INCREMENT, FALSE);
    }

    IoReleaseCancelSpinLock(Irp->CancelIrql);
}

NTSTATUS
NTAPI
NduDispatchRead(PDEVICE_OBJECT DeviceObject,
                PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    BOOLEAN WriteOnly = FALSE;
    KIRQL OldIrql, OldCancelIrql;
    NTSTATUS Status;
    PLIST_ENTRY ListEntry;
    PNDISUIO_PACKET_ENTRY PacketEntry = NULL;
    ULONG BytesCopied = 0;

    ASSERT(DeviceObject == GlobalDeviceObject);

    /* The file object must be opened on an adapter, for reading. The adapter
     * stays until NduEndIo. */
    AdapterContext = NduBeginIo(IrpSp->FileObject, &WriteOnly);
    if (AdapterContext == NULL || WriteOnly)
    {
        if (AdapterContext != NULL)
            NduEndIo(AdapterContext);

        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_INVALID_PARAMETER;
    }

    /* Make the read cancellable */
    Irp->Tail.Overlay.DriverContext[0] = AdapterContext;
    IoAcquireCancelSpinLock(&OldCancelIrql);
    if (Irp->Cancel)
    {
        IoReleaseCancelSpinLock(OldCancelIrql);
        NduEndIo(AdapterContext);

        /* Indicate a 0 byte read */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_SUCCESS;
    }
    IoSetCancelRoutine(Irp, ReadIrpCancel);
    IoReleaseCancelSpinLock(OldCancelIrql);

    while (TRUE)
    {
        KeAcquireSpinLock(&AdapterContext->Spinlock, &OldIrql);

        /* The adapter is being unbound: leave, and wake the next reader */
        if (AdapterContext->Unbinding)
        {
            KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

            /* Remove the cancel routine */
            IoAcquireCancelSpinLock(&OldCancelIrql);
            IoSetCancelRoutine(Irp, NULL);
            IoReleaseCancelSpinLock(OldCancelIrql);

            KeSetEvent(&AdapterContext->PacketReadEvent, IO_NO_INCREMENT, FALSE);
            Status = STATUS_CANCELLED;
            break;
        }

        /* Check if we have a packet */
        if (IsListEmpty(&AdapterContext->PacketList))
        {
            KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

            /* Wait for a packet (in the context of the calling user thread) */
            Status = KeWaitForSingleObject(&AdapterContext->PacketReadEvent,
                                           UserRequest,
                                           UserMode,
                                           TRUE,
                                           NULL);
            if (Status != STATUS_SUCCESS)
            {
                /* Remove the cancel routine */
                IoAcquireCancelSpinLock(&OldCancelIrql);
                IoSetCancelRoutine(Irp, NULL);
                IoReleaseCancelSpinLock(OldCancelIrql);

                break;
            }
        }
        else
        {
            /* Remove the first packet in the list */
            ListEntry = RemoveHeadList(&AdapterContext->PacketList);
            PacketEntry = CONTAINING_RECORD(ListEntry, NDISUIO_PACKET_ENTRY, ListEntry);

            /* The empty entry of a cancelled read is not counted */
            if (PacketEntry->PacketLength != 0)
                AdapterContext->PacketCount--;

            /* Release the adapter lock */
            KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

            /* Remove the cancel routine. The cancel routine takes the adapter
             * lock with the cancel spin lock held, so the cancel spin lock
             * is never taken with the adapter lock held. */
            IoAcquireCancelSpinLock(&OldCancelIrql);
            IoSetCancelRoutine(Irp, NULL);
            IoReleaseCancelSpinLock(OldCancelIrql);

            /* And we're done with this loop */
            Status = STATUS_SUCCESS;
            break;
        }
    }

    /* Check if we got a packet */
    if (PacketEntry != NULL)
    {
        /* Find the right amount of bytes to copy */
        BytesCopied = PacketEntry->PacketLength;
        if (BytesCopied > IrpSp->Parameters.Read.Length)
            BytesCopied = IrpSp->Parameters.Read.Length;

        /* Copy the packet */
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                      &PacketEntry->PacketData[0],
                      BytesCopied);

        /* Free the packet entry */
        ExFreePool(PacketEntry);
    }
    else
    {
        /* Something failed */
        BytesCopied = 0;
    }

    NduEndIo(AdapterContext);

    /* Complete the IRP */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = BytesCopied;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return Status;
}

NTSTATUS
NTAPI
NduDispatchWrite(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNET_BUFFER_LIST Nbl = NULL;
    PMDL Mdl;
    KEVENT SendEvent;
    KIRQL OldIrql;
    NDIS_STATUS Status;
    ULONG BytesCopied = 0;

    ASSERT(DeviceObject == GlobalDeviceObject);

    /* The file object must be opened on an adapter; both kinds of open may
     * write. The adapter stays until NduEndIo. */
    AdapterContext = NduBeginIo(IrpSp->FileObject, NULL);
    if (AdapterContext == NULL)
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&AdapterContext->Spinlock, &OldIrql);
    if (AdapterContext->Paused)
    {
        KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);
        NduEndIo(AdapterContext);

        Irp->IoStatus.Status = NDIS_STATUS_PAUSED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return NDIS_STATUS_PAUSED;
    }
    AdapterContext->PendingSends++;
    KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

    /* Create a packet and buffer descriptor for this user buffer */
    Mdl = NdisAllocateMdl(AdapterContext->BindingHandle,
                          Irp->AssociatedIrp.SystemBuffer,
                          IrpSp->Parameters.Write.Length);
    if (Mdl)
    {
        Nbl = NdisAllocateNetBufferAndNetBufferList(AdapterContext->NblPoolHandle,
                                                    0,
                                                    0,
                                                    Mdl,
                                                    0,
                                                    IrpSp->Parameters.Write.Length);
    }
    if (Nbl)
    {
        KeInitializeEvent(&SendEvent, NotificationEvent, FALSE);
        Nbl->ProtocolReserved[0] = &SendEvent;
        Nbl->SourceHandle = AdapterContext->BindingHandle;

        /* Send it via NDIS */
        NdisSendNetBufferLists(AdapterContext->BindingHandle,
                               Nbl,
                               NDIS_DEFAULT_PORT_NUMBER,
                               0);

        /* Wait for the send */
        KeWaitForSingleObject(&SendEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = NET_BUFFER_LIST_STATUS(Nbl);

        /* Check if it succeeded */
        if (Status == NDIS_STATUS_SUCCESS)
            BytesCopied = IrpSp->Parameters.Write.Length;

        NdisFreeNetBufferList(Nbl);
    }
    else
    {
        /* No memory */
        Status = STATUS_NO_MEMORY;
    }

    if (Mdl)
        NdisFreeMdl(Mdl);

    KeAcquireSpinLock(&AdapterContext->Spinlock, &OldIrql);
    if (--AdapterContext->PendingSends == 0 && AdapterContext->Paused)
        KeSetEvent(&AdapterContext->SendsDoneEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

    NduEndIo(AdapterContext);

    /* Complete the IRP */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = BytesCopied;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return Status;
}
