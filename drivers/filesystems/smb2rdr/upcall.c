/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel -> daemon request transport
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The daemon blocks in IOCTL_SMB2RDR_GETREQ (waking every second to notice a dying thread or a
 * shutdown), receives one request, does the SMB2 work and answers with IOCTL_SMB2RDR_PUTREPLY.
 * A request lives in NonPagedPool, on exactly one of two lists: Queued (waiting to be fetched)
 * or Inflight (fetched, waiting for the reply). The waiter owns and frees the entry; the reply
 * is copied into the entry's own nonpaged buffer while the entry is still on the Inflight list
 * under the lock, so a timed-out waiter that already removed the entry can never be written to.
 */

#include "smb2rdr.h"

typedef struct _SMB2_REQUEST
{
    LIST_ENTRY Link;
    ULONGLONG Id;
    ULONG Opcode;
    PUCHAR Payload;             /* nonpaged copy of the request payload */
    ULONG PayloadLength;
    PUCHAR Reply;               /* nonpaged reply buffer of capacity ReplyCapacity */
    ULONG ReplyCapacity;
    ULONG ReplyLength;
    NTSTATUS Status;
    PACCESS_TOKEN Token;        /* referenced; the caller identity the daemon may impersonate */
    KEVENT Done;
} SMB2_REQUEST, *PSMB2_REQUEST;

static KSPIN_LOCK UpcallLock;
static LIST_ENTRY QueuedList;
static LIST_ENTRY InflightList;
static KEVENT QueueEvent;               /* synchronization event: a request was queued */
static volatile LONGLONG NextId;
static volatile LONG DaemonRunning;
static volatile LONG ShuttingDown;

VOID Smb2UpcallInit(VOID)
{
    KeInitializeSpinLock(&UpcallLock);
    InitializeListHead(&QueuedList);
    InitializeListHead(&InflightList);
    KeInitializeEvent(&QueueEvent, SynchronizationEvent, FALSE);
    NextId = 0;
    DaemonRunning = 0;
    ShuttingDown = 0;
}

VOID Smb2UpcallShutdown(VOID)
{
    InterlockedExchange(&ShuttingDown, 1);
    InterlockedExchange(&DaemonRunning, 0);
    KeSetEvent(&QueueEvent, IO_NO_INCREMENT, FALSE);
}

BOOLEAN Smb2UpcallDaemonRunning(VOID)
{
    return DaemonRunning != 0;
}

VOID Smb2UpcallSetRunning(BOOLEAN Running)
{
    InterlockedExchange(&DaemonRunning, Running ? 1 : 0);
    if (Running)
        InterlockedExchange(&ShuttingDown, 0);
}

static VOID FreeRequest(PSMB2_REQUEST Req)
{
    if (Req->Payload)
        ExFreePoolWithTag(Req->Payload, SMB2RDR_TAG_UP);
    if (Req->Reply)
        ExFreePoolWithTag(Req->Reply, SMB2RDR_TAG_UP);
    if (Req->Token)
        ObDereferenceObject(Req->Token);
    ExFreePoolWithTag(Req, SMB2RDR_TAG_UP);
}

NTSTATUS Smb2Upcall(ULONG Opcode, const VOID *Payload, ULONG PayloadLength,
                    PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength)
{
    return Smb2UpcallEx(Opcode, Payload, PayloadLength, Reply, ReplyCapacity, ReplyLength, NULL);
}

NTSTATUS Smb2UpcallEx(ULONG Opcode, const VOID *Payload, ULONG PayloadLength,
                      PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength, PACCESS_TOKEN Token)
{
    PSMB2_REQUEST req;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    KIRQL irql;

    if (ReplyLength)
        *ReplyLength = 0;
    if (!DaemonRunning)
        return STATUS_REDIRECTOR_NOT_STARTED;
    if (PayloadLength > SMB2RDR_MAX_PAYLOAD || ReplyCapacity > SMB2RDR_MAX_PAYLOAD)
        return STATUS_INVALID_PARAMETER;

    req = ExAllocatePoolWithTag(NonPagedPool, sizeof(*req), SMB2RDR_TAG_UP);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(req, sizeof(*req));
    req->Id = (ULONGLONG)InterlockedIncrement64(&NextId);
    req->Opcode = Opcode;
    req->PayloadLength = PayloadLength;
    req->ReplyCapacity = ReplyCapacity;
    if (Token)
    {
        ObReferenceObject(Token);
        req->Token = Token;
    }
    KeInitializeEvent(&req->Done, NotificationEvent, FALSE);
    if (PayloadLength)
    {
        req->Payload = ExAllocatePoolWithTag(NonPagedPool, PayloadLength, SMB2RDR_TAG_UP);
        if (!req->Payload)
        {
            FreeRequest(req);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(req->Payload, Payload, PayloadLength);
    }
    if (ReplyCapacity)
    {
        req->Reply = ExAllocatePoolWithTag(NonPagedPool, ReplyCapacity, SMB2RDR_TAG_UP);
        if (!req->Reply)
        {
            FreeRequest(req);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    KeAcquireSpinLock(&UpcallLock, &irql);
    InsertTailList(&QueuedList, &req->Link);
    KeReleaseSpinLock(&UpcallLock, irql);
    KeSetEvent(&QueueEvent, IO_NO_INCREMENT, FALSE);

    timeout.QuadPart = -(LONGLONG)SMB2RDR_UPCALL_TIMEOUT_SEC * 10000000LL;
    status = KeWaitForSingleObject(&req->Done, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT)
    {
        /* Withdraw the request. If the reply raced us and already completed it, take the result. */
        KeAcquireSpinLock(&UpcallLock, &irql);
        if (!KeReadStateEvent(&req->Done))
        {
            RemoveEntryList(&req->Link);
            KeReleaseSpinLock(&UpcallLock, irql);
            FreeRequest(req);
            return STATUS_IO_TIMEOUT;
        }
        KeReleaseSpinLock(&UpcallLock, irql);
    }

    status = req->Status;
    if (NT_SUCCESS(status) && req->ReplyLength)
    {
        if (Reply)
            RtlCopyMemory(Reply, req->Reply, req->ReplyLength);
        if (ReplyLength)
            *ReplyLength = req->ReplyLength;
    }
    FreeRequest(req);
    return status;
}

/*
 * An upcall the daemon may hold for as long as the caller waits (a blocking byte-range lock). The wait is
 * alertable and ends early when the IRP is cancelled or the thread is terminating: the daemon is then
 * told to cancel the request (SMB2RDR_UP_CANCEL) and answers the upcall itself, normally STATUS_CANCELLED.
 * If it does not answer within SMB2RDR_UPCALL_TIMEOUT_SEC the request is withdrawn.
 */
NTSTATUS Smb2UpcallCancellable(ULONG Opcode, const VOID *Payload, ULONG PayloadLength, PIRP Irp,
                               PVOID Reply, ULONG ReplyCapacity, PULONG ReplyLength, volatile LONGLONG *PublishedId)
{
    PSMB2_REQUEST req;
    LARGE_INTEGER slice;
    NTSTATUS status;
    KIRQL irql;
    BOOLEAN cancelSent = FALSE;
    ULONG waitedAfterCancel = 0;

    if (ReplyLength)
        *ReplyLength = 0;
    if (!DaemonRunning)
        return STATUS_REDIRECTOR_NOT_STARTED;
    if (PayloadLength > SMB2RDR_MAX_PAYLOAD || ReplyCapacity > SMB2RDR_MAX_PAYLOAD)
        return STATUS_INVALID_PARAMETER;

    req = ExAllocatePoolWithTag(NonPagedPool, sizeof(*req), SMB2RDR_TAG_UP);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(req, sizeof(*req));
    req->Id = (ULONGLONG)InterlockedIncrement64(&NextId);
    req->Opcode = Opcode;
    req->PayloadLength = PayloadLength;
    req->ReplyCapacity = ReplyCapacity;
    KeInitializeEvent(&req->Done, NotificationEvent, FALSE);
    if (ReplyCapacity)
    {
        req->Reply = ExAllocatePoolWithTag(NonPagedPool, ReplyCapacity, SMB2RDR_TAG_UP);
        if (!req->Reply)
        {
            FreeRequest(req);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (PayloadLength)
    {
        req->Payload = ExAllocatePoolWithTag(NonPagedPool, PayloadLength, SMB2RDR_TAG_UP);
        if (!req->Payload)
        {
            FreeRequest(req);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(req->Payload, Payload, PayloadLength);
    }

    if (PublishedId)
        InterlockedExchange64(PublishedId, (LONGLONG)req->Id);
    KeAcquireSpinLock(&UpcallLock, &irql);
    InsertTailList(&QueuedList, &req->Link);
    KeReleaseSpinLock(&UpcallLock, irql);
    KeSetEvent(&QueueEvent, IO_NO_INCREMENT, FALSE);

    slice.QuadPart = -(LONGLONG)SMB2RDR_LOCK_POLL_MS * 10000;
    for (;;)
    {
        status = KeWaitForSingleObject(&req->Done, Executive, UserMode, TRUE, &slice);
        if (status == STATUS_SUCCESS)
            break;

        if (!cancelSent && (status != STATUS_TIMEOUT || (Irp && Irp->Cancel) || PsIsThreadTerminating(PsGetCurrentThread())))
        {
            SMB2RDR_CANCEL_REQ cancel;

            cancel.TargetId = req->Id;
            cancelSent = TRUE;
            Smb2Upcall(SMB2RDR_UP_CANCEL, &cancel, sizeof(cancel), NULL, 0, NULL);
            continue;
        }
        if (cancelSent)
            waitedAfterCancel += SMB2RDR_LOCK_POLL_MS;
        if (!DaemonRunning || waitedAfterCancel >= SMB2RDR_UPCALL_TIMEOUT_SEC * 1000)
        {
            KeAcquireSpinLock(&UpcallLock, &irql);
            if (!KeReadStateEvent(&req->Done))
            {
                RemoveEntryList(&req->Link);
                KeReleaseSpinLock(&UpcallLock, irql);
                if (PublishedId)
                    InterlockedExchange64(PublishedId, 0);
                FreeRequest(req);
                return cancelSent ? STATUS_CANCELLED : STATUS_REDIRECTOR_NOT_STARTED;
            }
            KeReleaseSpinLock(&UpcallLock, irql);
            break;
        }
    }

    if (PublishedId)
        InterlockedExchange64(PublishedId, 0);
    status = req->Status;
    if (NT_SUCCESS(status) && req->ReplyLength)
    {
        if (Reply)
            RtlCopyMemory(Reply, req->Reply, req->ReplyLength);
        if (ReplyLength)
            *ReplyLength = req->ReplyLength;
    }
    FreeRequest(req);
    return status;
}

/*
 * IOCTL_SMB2RDR_GETREQ: output buffer receives SMB2RDR_UPCALL + payload of the oldest queued request.
 * Blocks until a request is available.
 */
NTSTATUS Smb2HandleGetRequest(PVOID OutBuffer, ULONG OutLength, PULONG Written, ULONG RequestorPid)
{
    LARGE_INTEGER tick;
    KIRQL irql;

    *Written = 0;
    if (OutLength < sizeof(SMB2RDR_UPCALL))
        return STATUS_BUFFER_TOO_SMALL;
    tick.QuadPart = -10000000LL;        /* 1 s */

    for (;;)
    {
        PSMB2_REQUEST req = NULL;
        PACCESS_TOKEN token = NULL;
        NTSTATUS wait;

        KeAcquireSpinLock(&UpcallLock, &irql);
        if (!IsListEmpty(&QueuedList))
        {
            req = CONTAINING_RECORD(QueuedList.Flink, SMB2_REQUEST, Link);
            if (OutLength < sizeof(SMB2RDR_UPCALL) + req->PayloadLength)
            {
                KeReleaseSpinLock(&UpcallLock, irql);
                return STATUS_BUFFER_TOO_SMALL;
            }
            RemoveEntryList(&req->Link);
            InsertTailList(&InflightList, &req->Link);
            {
                SMB2RDR_UPCALL *up = (SMB2RDR_UPCALL *)OutBuffer;

                up->Opcode = req->Opcode;
                up->Length = req->PayloadLength;
                up->Id = req->Id;
                up->TokenHandle = 0;
                token = req->Token;
                if (token)
                    ObReferenceObject(token);   /* the waiter may time out and drop its own */
                if (req->PayloadLength)
                    RtlCopyMemory(up + 1, req->Payload, req->PayloadLength);
                *Written = sizeof(*up) + req->PayloadLength;
            }
            /* More queued? let the next GETREQ run without waiting */
            if (!IsListEmpty(&QueuedList))
                KeSetEvent(&QueueEvent, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&UpcallLock, irql);
            if (token)
            {
                /* Hand the daemon an impersonation token handle. It is only meaningful in the daemon's own
                 * process, so only when this IOCTL really runs there; without one the daemon fails closed
                 * rather than connecting as itself. */
                if ((HANDLE)(ULONG_PTR)RequestorPid == PsGetCurrentProcessId())
                {
                    HANDLE handle = NULL;

                    if (NT_SUCCESS(ObOpenObjectByPointer(token, 0, NULL, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                                                         *SeTokenObjectType, KernelMode, &handle)))
                        ((SMB2RDR_UPCALL *)OutBuffer)->TokenHandle = (ULONGLONG)(ULONG_PTR)handle;
                }
                ObDereferenceObject(token);
            }
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&UpcallLock, irql);

        if (ShuttingDown || !DaemonRunning)
            return STATUS_CANCELLED;
        if (PsIsThreadTerminating(PsGetCurrentThread()))
            return STATUS_THREAD_IS_TERMINATING;
        wait = KeWaitForSingleObject(&QueueEvent, Executive, KernelMode, FALSE, &tick);
        (void)wait;
    }
}

/* IOCTL_SMB2RDR_PUTREPLY */
NTSTATUS Smb2HandlePutReply(const VOID *InBuffer, ULONG InLength)
{
    const SMB2RDR_DOWNCALL *down = (const SMB2RDR_DOWNCALL *)InBuffer;
    PLIST_ENTRY entry;
    KIRQL irql;
    NTSTATUS status = STATUS_NOT_FOUND;

    if (InLength < sizeof(*down) || down->Length > InLength - sizeof(*down))
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&UpcallLock, &irql);
    for (entry = InflightList.Flink; entry != &InflightList; entry = entry->Flink)
    {
        PSMB2_REQUEST req = CONTAINING_RECORD(entry, SMB2_REQUEST, Link);

        if (req->Id != down->Id)
            continue;
        RemoveEntryList(&req->Link);
        req->Status = down->Status;
        req->ReplyLength = 0;
        if (NT_SUCCESS(down->Status) && down->Length)
        {
            /* A reply larger than the caller asked for is truncated, not trusted. */
            ULONG n = down->Length < req->ReplyCapacity ? down->Length : req->ReplyCapacity;

            if (n)
                RtlCopyMemory(req->Reply, down + 1, n);
            req->ReplyLength = n;
        }
        KeSetEvent(&req->Done, IO_NO_INCREMENT, FALSE);
        status = STATUS_SUCCESS;
        break;
    }
    KeReleaseSpinLock(&UpcallLock, irql);
    return status;
}
