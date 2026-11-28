/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        ioctl.c
 * PURPOSE:     IOCTL handling
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

//#define NDEBUG
#include <debug.h>

static
NTSTATUS
WaitForBind(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    /* I've seen several code samples that use this IOCTL but there's
     * no official documentation on it. I'm just implementing it as a no-op
     * right now because I don't see any reason we need it. We handle an open
     * and bind just fine with IRP_MJ_CREATE and IOCTL_NDISUIO_OPEN_DEVICE */
    DPRINT("Wait for bind complete\n");

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

static
NTSTATUS
QueryBinding(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = NULL;
    PNDISUIO_QUERY_BINDING QueryBinding = Irp->AssociatedIrp.SystemBuffer;
    ULONG BindingLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    NTSTATUS Status;
    PLIST_ENTRY CurrentEntry;
    KIRQL OldIrql;
    ULONG i;
    ULONG BytesCopied = 0;

    /* The answer is written over the request and is copied to the caller
     * for the length reported, so it must fit both buffers */
    if (BindingLength > IrpSp->Parameters.DeviceIoControl.OutputBufferLength)
        BindingLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

    if (QueryBinding && BindingLength >= sizeof(NDISUIO_QUERY_BINDING))
    {
        /* The adapter can be unbound and freed as soon as the list lock is
         * released, so its name is copied with the lock held */
        KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
        i = 0;
        CurrentEntry = GlobalAdapterList.Flink;
        while (CurrentEntry != &GlobalAdapterList)
        {
            if (i == QueryBinding->BindingIndex)
            {
                AdapterContext = CONTAINING_RECORD(CurrentEntry, NDISUIO_ADAPTER_CONTEXT, ListEntry);
                break;
            }
            i++;
            CurrentEntry = CurrentEntry->Flink;
        }
        if (AdapterContext)
        {
            BytesCopied = sizeof(NDISUIO_QUERY_BINDING);
            if (AdapterContext->DeviceName.Length <= BindingLength - BytesCopied)
            {
                QueryBinding->DeviceNameOffset = BytesCopied;
                QueryBinding->DeviceNameLength = AdapterContext->DeviceName.Length;
                RtlCopyMemory((PUCHAR)QueryBinding + QueryBinding->DeviceNameOffset,
                              AdapterContext->DeviceName.Buffer,
                              QueryBinding->DeviceNameLength);
                BytesCopied += AdapterContext->DeviceName.Length;

                /* FIXME: Copy description too */
                QueryBinding->DeviceDescrOffset = BytesCopied;
                QueryBinding->DeviceDescrLength = 0;

                /* Successful */
                Status = STATUS_SUCCESS;
            }
            else
            {
                /* Not enough buffer space */
                Status = STATUS_BUFFER_TOO_SMALL;
            }
        }
        else
        {
            /* Invalid index */
            Status = STATUS_NO_MORE_ENTRIES;
        }
        KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);
    }
    else
    {
        /* Invalid parameters */
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = BytesCopied;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

#if 0
static
NTSTATUS
CancelPacketRead(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = IrpSp->FileObject->FsContext;
    PNDISUIO_PACKET_ENTRY PacketEntry;
    NTSTATUS Status;

    /* Indicate a 0-byte packet on the queue so one read returns 0 */
    PacketEntry = ExAllocatePool(PagedPool, sizeof(NDISUIO_PACKET_ENTRY));
    if (PacketEntry)
    {
        PacketEntry->PacketLength = 0;

        ExInterlockedInsertHeadList(&AdapterContext->PacketList,
                                    &PacketEntry->ListEntry,
                                    &AdapterContext->Spinlock);

        KeSetEvent(&AdapterContext->PacketReadEvent, IO_NO_INCREMENT, FALSE);

        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_NO_MEMORY;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}
#endif

static
NTSTATUS
SetAdapterOid(PNDISUIO_ADAPTER_CONTEXT AdapterContext, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PNDISUIO_SET_OID SetOidRequest;
    NDISUIO_REQUEST UioRequest;
    PNDIS_OID_REQUEST Request = &UioRequest.Request;
    ULONG RequestLength;
    NDIS_STATUS Status;

    Irp->IoStatus.Information = 0;

    SetOidRequest = Irp->AssociatedIrp.SystemBuffer;
    RequestLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    if (SetOidRequest && RequestLength >= sizeof(NDIS_OID))
    {
        /* Setup the NDIS request */
        RtlZeroMemory(&UioRequest, sizeof(UioRequest));
        Request->RequestType = NdisRequestSetInformation;
        Request->DATA.SET_INFORMATION.Oid = SetOidRequest->Oid;
        Request->DATA.SET_INFORMATION.InformationBufferLength = RequestLength - sizeof(NDIS_OID);
        if (Request->DATA.SET_INFORMATION.InformationBufferLength != 0)
        {
            Request->DATA.SET_INFORMATION.InformationBuffer = SetOidRequest->Data;
        }
        else
        {
            Request->DATA.SET_INFORMATION.InformationBuffer = NULL;
        }
        Request->DATA.SET_INFORMATION.BytesRead = 0;

        DPRINT("Setting OID 0x%x on adapter %wZ\n", SetOidRequest->Oid, &AdapterContext->DeviceName);

        /* Dispatch the request */
        Status = NduDoRequest(AdapterContext, &UioRequest);

        /* Return the bytes read */
        if (Status == NDIS_STATUS_INVALID_LENGTH ||
            Status == NDIS_STATUS_BUFFER_TOO_SHORT)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else if (Status == NDIS_STATUS_SUCCESS)
        {
            /* The request buffer is copied back to the caller for the length
             * reported here, so it is limited to what the caller can take and
             * to what the request holds */
            Irp->IoStatus.Information = sizeof(NDIS_OID) +
                min(Request->DATA.SET_INFORMATION.BytesRead, RequestLength - sizeof(NDIS_OID));
            if (Irp->IoStatus.Information > IrpSp->Parameters.DeviceIoControl.OutputBufferLength)
                Irp->IoStatus.Information = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
        }

        DPRINT("Final request status: 0x%x (%d)\n", Status, Irp->IoStatus.Information);
    }
    else
    {
        /* Bad parameters */
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

static
NTSTATUS
QueryAdapterOid(PNDISUIO_ADAPTER_CONTEXT AdapterContext, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PNDISUIO_QUERY_OID QueryOidRequest;
    NDISUIO_REQUEST UioRequest;
    PNDIS_OID_REQUEST Request = &UioRequest.Request;
    ULONG RequestLength;
    NDIS_STATUS Status;

    Irp->IoStatus.Information = 0;

    QueryOidRequest = Irp->AssociatedIrp.SystemBuffer;
    RequestLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

    /* The answer is written over the request and is copied to the caller
     * for the length reported, so the adapter is given only the room that
     * both buffers have */
    if (RequestLength > IrpSp->Parameters.DeviceIoControl.OutputBufferLength)
        RequestLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

    if (QueryOidRequest && RequestLength >= sizeof(NDIS_OID))
    {
        /* Setup the NDIS request */
        RtlZeroMemory(&UioRequest, sizeof(UioRequest));
        Request->RequestType = NdisRequestQueryInformation;
        Request->DATA.QUERY_INFORMATION.Oid = QueryOidRequest->Oid;
        Request->DATA.QUERY_INFORMATION.InformationBufferLength = RequestLength - sizeof(NDIS_OID);
        if (Request->DATA.QUERY_INFORMATION.InformationBufferLength != 0)
        {
            Request->DATA.QUERY_INFORMATION.InformationBuffer = QueryOidRequest->Data;
        }
        else
        {
            Request->DATA.QUERY_INFORMATION.InformationBuffer = NULL;
        }
        Request->DATA.QUERY_INFORMATION.BytesWritten = 0;

        DPRINT("Querying OID 0x%x on adapter %wZ\n", QueryOidRequest->Oid, &AdapterContext->DeviceName);

        /* Dispatch the request */
        Status = NduDoRequest(AdapterContext, &UioRequest);

        /* Return the bytes written */
        if (Status == NDIS_STATUS_INVALID_LENGTH ||
            Status == NDIS_STATUS_BUFFER_TOO_SHORT)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else if (Status == NDIS_STATUS_SUCCESS)
        {
            Irp->IoStatus.Information = sizeof(NDIS_OID) +
                min(Request->DATA.QUERY_INFORMATION.BytesWritten, RequestLength - sizeof(NDIS_OID));
        }

        DPRINT("Final request status: 0x%x (%d)\n", Status, Irp->IoStatus.Information);
    }
    else
    {
        /* Bad parameters */
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

/* Compares two adapter names without regard to the case of the letters A to Z.
 * It runs with the adapter list lock held, where the string routines of the
 * system, which use the case tables, must not be called. */
static
BOOLEAN
NduEqualAdapterName(PCUNICODE_STRING Name1, PCUNICODE_STRING Name2)
{
    USHORT i;
    WCHAR Char1, Char2;

    if (Name1->Length != Name2->Length)
        return FALSE;

    for (i = 0; i < Name1->Length / sizeof(WCHAR); i++)
    {
        Char1 = Name1->Buffer[i];
        Char2 = Name2->Buffer[i];
        if (Char1 >= L'a' && Char1 <= L'z')
            Char1 -= L'a' - L'A';
        if (Char2 >= L'a' && Char2 <= L'z')
            Char2 -= L'a' - L'A';
        if (Char1 != Char2)
            return FALSE;
    }

    return TRUE;
}

static
NTSTATUS
OpenDeviceReadWrite(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    UNICODE_STRING DeviceName;
    ULONG NameLength;
    NTSTATUS Status;
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = NULL;
    PNDISUIO_OPEN_ENTRY OpenEntry;
    PLIST_ENTRY CurrentEntry;
    KIRQL OldIrql;

    NameLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength & ~(sizeof(WCHAR) - 1);
    if (NameLength != 0 && NameLength <= UNICODE_STRING_MAX_BYTES)
    {
        DeviceName.MaximumLength = DeviceName.Length = (USHORT)NameLength;
        DeviceName.Buffer = Irp->AssociatedIrp.SystemBuffer;

        OpenEntry = ExAllocatePool(NonPagedPool, sizeof(*OpenEntry));
        if (OpenEntry)
        {
            /* Set the file object pointer */
            OpenEntry->FileObject = FileObject;

            /* Set the permissions */
            OpenEntry->WriteOnly = FALSE;

            /* The adapter is looked up and the file object is associated
             * with it under the list lock, so an unbind sees both or none */
            KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
            CurrentEntry = GlobalAdapterList.Flink;
            while (CurrentEntry != &GlobalAdapterList)
            {
                AdapterContext = CONTAINING_RECORD(CurrentEntry, NDISUIO_ADAPTER_CONTEXT, ListEntry);
                if (NduEqualAdapterName(&AdapterContext->DeviceName, &DeviceName))
                    break;

                AdapterContext = NULL;
                CurrentEntry = CurrentEntry->Flink;
            }

            Status = STATUS_INVALID_PARAMETER;
            if (AdapterContext != NULL && FileObject->FsContext == NULL)
            {
                KeAcquireSpinLockAtDpcLevel(&AdapterContext->Spinlock);

                /* An open for read-write is exclusive,
                 * so we can't have any other open handles */
                if (AdapterContext->OpenCount == 0)
                {
                    /* Add a reference */
                    ReferenceAdapterContext(AdapterContext);

                    /* Associate this FO with the adapter */
                    FileObject->FsContext = AdapterContext;
                    FileObject->FsContext2 = OpenEntry;

                    /* Add it to the adapter's list */
                    InsertTailList(&AdapterContext->OpenEntryList,
                                   &OpenEntry->ListEntry);

                    Status = STATUS_SUCCESS;
                }

                KeReleaseSpinLockFromDpcLevel(&AdapterContext->Spinlock);
            }
            KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);

            if (!NT_SUCCESS(Status))
                ExFreePool(OpenEntry);
        }
        else
        {
            Status = STATUS_NO_MEMORY;
        }
    }
    else
    {
        /* Invalid device name */
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

#if 0
static
NTSTATUS
OpenDeviceWrite(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    UNICODE_STRING DeviceName;
    ULONG NameLength;
    NTSTATUS Status;
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNDISUIO_OPEN_ENTRY OpenEntry;
    KIRQL OldIrql;

    NameLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    if (NameLength != 0)
    {
        DeviceName.MaximumLength = DeviceName.Length = NameLength;
        DeviceName.Buffer = Irp->AssociatedIrp.SystemBuffer;

        /* Check if this already has a context */
        AdapterContext = FindAdapterContextByName(&DeviceName);
        if (AdapterContext != NULL)
        {
            /* Reference the adapter context */
            KeAcquireSpinLock(&AdapterContext->Spinlock, &OldIrql);
            ReferenceAdapterContext(AdapterContext);
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Invalid device name */
            Status = STATUS_INVALID_PARAMETER;
        }

        /* Check that the bind succeeded */
        if (NT_SUCCESS(Status))
        {
            OpenEntry = ExAllocatePool(NonPagedPool, sizeof(*OpenEntry));
            if (OpenEntry)
            {
                /* Set the file object pointer */
                OpenEntry->FileObject = FileObject;

                /* Associate this FO with the adapter */
                FileObject->FsContext = AdapterContext;
                FileObject->FsContext2 = OpenEntry;

                /* Set permissions */
                OpenEntry->WriteOnly = TRUE;

                /* Add it to the adapter's list */
                InsertTailList(&AdapterContext->OpenEntryList,
                               &OpenEntry->ListEntry);

                /* Success */
                KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);
                Status = STATUS_SUCCESS;
            }
            else
            {
                /* Remove the reference we added */
                KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);
                DereferenceAdapterContext(AdapterContext, NULL);
                Status = STATUS_NO_MEMORY;
            }
        }
    }
    else
    {
        /* Invalid device name */
        Status = STATUS_INVALID_PARAMETER;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}
#endif

NTSTATUS
NTAPI
NduDispatchDeviceControl(PDEVICE_OBJECT DeviceObject,
                         PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    BOOLEAN WriteOnly = FALSE;
    NTSTATUS Status;

    ASSERT(DeviceObject == GlobalDeviceObject);

    /* Handle open IOCTLs first */
    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_NDISUIO_OPEN_DEVICE:
            return OpenDeviceReadWrite(Irp, IrpSp);
#if 0
        case IOCTL_NDISUIO_OPEN_WRITE_DEVICE:
            return OpenDeviceWrite(Irp, IrpSp);
#endif
        case IOCTL_NDISUIO_BIND_WAIT:
            return WaitForBind(Irp, IrpSp);

        case IOCTL_NDISUIO_QUERY_BINDING:
            return QueryBinding(Irp, IrpSp);

        default:
            /* Fail if this file object has no adapter associated. The
             * adapter stays until the request is done. */
            AdapterContext = NduBeginIo(IrpSp->FileObject, &WriteOnly);
            if (AdapterContext == NULL)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);

                return STATUS_INVALID_PARAMETER;
            }

            switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
            {
                /* Write IOCTLs */
                case IOCTL_NDISUIO_SET_OID_VALUE:
                    Status = SetAdapterOid(AdapterContext, Irp, IrpSp);
                    break;

                /* Read IOCTLs need read permissions */
                case IOCTL_NDISUIO_QUERY_OID_VALUE:
                    if (!WriteOnly)
                    {
                        Status = QueryAdapterOid(AdapterContext, Irp, IrpSp);
                        break;
                    }

                    Status = STATUS_INVALID_PARAMETER;
                    Irp->IoStatus.Status = Status;
                    Irp->IoStatus.Information = 0;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    break;

                default:
                    DPRINT1("Unimplemented\n");
                    Status = WriteOnly ? STATUS_INVALID_PARAMETER : STATUS_NOT_IMPLEMENTED;
                    Irp->IoStatus.Status = Status;
                    Irp->IoStatus.Information = 0;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    break;
            }

            NduEndIo(AdapterContext);
            return Status;
    }
}
