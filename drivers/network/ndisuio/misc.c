/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        misc.c
 * PURPOSE:     Helper functions
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

#define NDEBUG
#include <debug.h>

NDIS_STATUS
NduDoRequest(PNDISUIO_ADAPTER_CONTEXT AdapterContext,
             PNDISUIO_REQUEST Request)
{
    NDIS_STATUS Status;

    Request->Request.Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    Request->Request.Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    Request->Request.Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    KeInitializeEvent(&Request->Event, NotificationEvent, FALSE);

    Status = NdisOidRequest(AdapterContext->BindingHandle, &Request->Request);
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&Request->Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = Request->Status;
    }

    return Status;
}

/* Returns the adapter that the file object is opened on and keeps it alive
 * until NduEndIo. Returns NULL if the file object has no adapter, which is
 * also the case after the adapter was unbound. */
PNDISUIO_ADAPTER_CONTEXT
NduBeginIo(PFILE_OBJECT FileObject,
           PBOOLEAN WriteOnly)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNDISUIO_OPEN_ENTRY OpenEntry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
    AdapterContext = FileObject->FsContext;
    if (AdapterContext != NULL)
    {
        OpenEntry = FileObject->FsContext2;
        ASSERT(!AdapterContext->Unbinding);
        ASSERT(OpenEntry != NULL);

        AdapterContext->IoCount++;
        if (WriteOnly != NULL)
            *WriteOnly = OpenEntry->WriteOnly;
    }
    KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);

    return AdapterContext;
}

VOID
NduEndIo(PNDISUIO_ADAPTER_CONTEXT AdapterContext)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
    ASSERT(AdapterContext->IoCount > 0);
    if (--AdapterContext->IoCount == 0 && AdapterContext->Unbinding)
        KeSetEvent(&AdapterContext->IoDoneEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);
}

VOID
ReferenceAdapterContext(PNDISUIO_ADAPTER_CONTEXT AdapterContext)
{
    /* Increment the open count */
    AdapterContext->OpenCount++;
}
