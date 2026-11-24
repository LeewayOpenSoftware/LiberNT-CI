/*
 * PROJECT:     LiberNT win32k user-mode font driver channel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Registration and request/reply transport between win32k and fontdrvhost.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Locking: g_Umfd->Lock guards the channel fields only. It is never held across
 * a wait or a user-memory access. UmfdCallHost callers hold no USER, font-list,
 * face or cache lock; SERVE takes no lock but this one.
 */

#include <win32k.h>
#include <reactos/umfd.h>

DBG_DEFAULT_CHANNEL(GdiFont);

#define TAG_UMFD                'dfmU'
#define UMFD_REQUEST_TIMEOUT_MS 2000
#define UMFD_SERVE_WAIT_MS      30000

typedef enum _UMFD_SLOT_STATE
{
    UmfdSlotEmpty,
    UmfdSlotPosted,     /* waiting for SERVE to take it */
    UmfdSlotTaken,      /* delivered to the host, waiting for its reply */
    UmfdSlotDone        /* Reply set, or SlotStatus says why not */
} UMFD_SLOT_STATE;

typedef struct _UMFD_CHANNEL
{
    FAST_MUTEX Lock;
    PEPROCESS Host;             /* referenced while registered */
    BOOLEAN ServeActive;
    ULONG NextRequestId;
    KEVENT SlotFree;            /* one request at a time */
    KEVENT RequestPosted;       /* wakes the SERVE thread */
    KEVENT ReplyReady;          /* wakes the requester */
    UMFD_SLOT_STATE SlotState;
    ULONG SlotRequestId;
    USHORT SlotOp;
    NTSTATUS SlotStatus;
    PUMFD_HEADER Request;       /* owned by the slot until SERVE takes it */
    PUMFD_HEADER Reply;
} UMFD_CHANNEL, *PUMFD_CHANNEL;

/* One channel per win32k instance (single session today); waitable objects live in nonpaged pool */
static PUMFD_CHANNEL g_Umfd;
static ULONG g_UmfdMode;

VOID
UmfdInitialize(VOID)
{
    HKEY hKey;
    DWORD dwMode = 0;

    if (NT_SUCCESS(RegOpenKey(L"\\Registry\\Machine\\SOFTWARE\\Leeway\\Win32k", &hKey)))
    {
        if (!RegReadDWORD(hKey, L"UmfdMode", &dwMode))
            dwMode = 0;
        ZwClose(hKey);
    }

    /* Mode 2 (serve) needs a host identity the session user cannot open; not yet */
    if (dwMode != 0 && dwMode != 1)
    {
        ERR("UmfdMode %lu is not available, the channel stays off\n", dwMode);
        dwMode = 0;
    }
    if (dwMode != 0)
    {
        g_Umfd = ExAllocatePoolZero(NonPagedPool, sizeof(*g_Umfd), TAG_UMFD);
        if (g_Umfd == NULL)
        {
            ERR("UMFD channel allocation failed, the channel stays off\n");
            dwMode = 0;
        }
        else
        {
            ExInitializeFastMutex(&g_Umfd->Lock);
            KeInitializeEvent(&g_Umfd->SlotFree, SynchronizationEvent, TRUE);
            KeInitializeEvent(&g_Umfd->RequestPosted, SynchronizationEvent, FALSE);
            KeInitializeEvent(&g_Umfd->ReplyReady, NotificationEvent, FALSE);
            g_Umfd->NextRequestId = 1;
        }
    }
    g_UmfdMode = dwMode;
    ERR("UMFD channel mode %lu (read once at boot)\n", g_UmfdMode);
}

ULONG
UmfdGetMode(VOID)
{
    return g_UmfdMode;
}

/* Takes the channel down; returns the host reference for the caller to drop outside the lock */
static PEPROCESS
UmfdpChannelDownLocked(VOID)
{
    PEPROCESS Host = g_Umfd->Host;

    g_Umfd->Host = NULL;
    if (g_Umfd->SlotState == UmfdSlotPosted || g_Umfd->SlotState == UmfdSlotTaken)
    {
        /* Fail the outstanding request now, not at its timeout */
        g_Umfd->SlotState = UmfdSlotDone;
        g_Umfd->SlotStatus = STATUS_PORT_DISCONNECTED;
        KeSetEvent(&g_Umfd->ReplyReady, IO_NO_INCREMENT, FALSE);
    }
    /* Wake SERVE so it sees the channel down */
    KeSetEvent(&g_Umfd->RequestPosted, IO_NO_INCREMENT, FALSE);
    return Host;
}

/* A host that dies before it ever calls win32k gets no process callout */
static VOID
UmfdpDropDeadHostLocked(_Out_ PEPROCESS *Dead)
{
    *Dead = NULL;
    if (g_Umfd->Host && PsGetProcessExitStatus(g_Umfd->Host) != STATUS_PENDING)
        *Dead = UmfdpChannelDownLocked();
}

/* Sanity check only, never authorization: the image is \SystemRoot\System32\fontdrvhost.exe */
static BOOLEAN
UmfdpIsHostImage(_In_ PEPROCESS Process)
{
    UNICODE_STRING Expected = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\fontdrvhost.exe");
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE hFile;
    PFILE_OBJECT FileObject;
    POBJECT_NAME_INFORMATION ExpectedName = NULL;
    PUNICODE_STRING ImageName = NULL;
    ULONG Length = sizeof(OBJECT_NAME_INFORMATION) + 1024 * sizeof(WCHAR);
    BOOLEAN Match = FALSE;
    NTSTATUS Status;

    Status = SeLocateProcessImageName(Process, &ImageName);
    if (!NT_SUCCESS(Status))
        return FALSE;

    InitializeObjectAttributes(&Attributes, &Expected, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&hFile, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &Attributes, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(hFile, 0, *IoFileObjectType, KernelMode, (PVOID *)&FileObject, NULL);
        ZwClose(hFile);
        if (NT_SUCCESS(Status))
        {
            /* Both names come from ObQueryNameString, so both are device paths */
            ExpectedName = ExAllocatePoolWithTag(PagedPool, Length, TAG_UMFD);
            if (ExpectedName && NT_SUCCESS(ObQueryNameString(FileObject, ExpectedName, Length, &Length)))
                Match = RtlEqualUnicodeString(&ExpectedName->Name, ImageName, TRUE);
            ObDereferenceObject(FileObject);
        }
    }

    if (ExpectedName)
        ExFreePoolWithTag(ExpectedName, TAG_UMFD);
    ExFreePool(ImageName);
    return Match;
}

static BOOLEAN
UmfdpCallerHasTcb(VOID)
{
    return SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), UserMode);
}

static INT
UmfdpRegister(_In_ INT InSize, _In_opt_ LPSTR UnsafeIn)
{
    UMFD_REGISTER Reg;
    PEPROCESS Process, Dead;
    NTSTATUS Status = STATUS_SUCCESS;

    if (InSize != sizeof(Reg) || UnsafeIn == NULL)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    _SEH2_TRY
    {
        ProbeForRead(UnsafeIn, sizeof(Reg), 1);
        RtlCopyMemory(&Reg, UnsafeIn, sizeof(Reg));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return 0;
    }
    if (Reg.Magic != UMFD_MAGIC || Reg.Version != UMFD_VERSION)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Only the session launcher (winlogon, or wininit for session 0) may name the host */
    if (!UmfdpCallerHasTcb())
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }

    /* Every full-access handle carries this right; the limited one is not in a pre-Vista PROCESS_ALL_ACCESS */
    Status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Reg.ProcessHandle, PROCESS_QUERY_INFORMATION,
                                       *PsProcessType, UserMode, (PVOID *)&Process, NULL);
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return 0;
    }

    if (Process == PsGetCurrentProcess() ||
        PsGetProcessSessionId(Process) != PsGetCurrentProcessSessionId() ||
        PsGetProcessExitStatus(Process) != STATUS_PENDING ||
        !UmfdpIsHostImage(Process))
    {
        ObDereferenceObject(Process);
        EngSetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }

    ExAcquireFastMutex(&g_Umfd->Lock);
    UmfdpDropDeadHostLocked(&Dead);
    if (g_Umfd->Host)
    {
        Status = STATUS_ALREADY_REGISTERED;
    }
    else
    {
        g_Umfd->Host = Process;
        g_Umfd->ServeActive = FALSE;
        KeClearEvent(&g_Umfd->RequestPosted);
        Process = NULL;
    }
    ExReleaseFastMutex(&g_Umfd->Lock);

    if (Dead)
        ObDereferenceObject(Dead);
    if (Process)
        ObDereferenceObject(Process);
    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return 0;
    }
    return 1;
}

static INT
UmfdpUnregister(VOID)
{
    PEPROCESS Host = NULL;
    BOOLEAN IsHost, Allowed;

    ExAcquireFastMutex(&g_Umfd->Lock);
    IsHost = g_Umfd->Host != NULL && g_Umfd->Host == PsGetCurrentProcess();
    Allowed = g_Umfd->Host != NULL &&
              (IsHost || PsGetProcessSessionId(g_Umfd->Host) == PsGetCurrentProcessSessionId());
    ExReleaseFastMutex(&g_Umfd->Lock);

    /* The privilege check may touch the token: do it outside the lock */
    if (Allowed && !IsHost && !UmfdpCallerHasTcb())
        Allowed = FALSE;
    if (!Allowed)
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }

    ExAcquireFastMutex(&g_Umfd->Lock);
    if (g_Umfd->Host)
        Host = UmfdpChannelDownLocked();
    ExReleaseFastMutex(&g_Umfd->Lock);

    if (Host)
        ObDereferenceObject(Host);
    return 1;
}

static BOOLEAN
UmfdpIsValidHeader(_In_ const UMFD_HEADER *Header, _In_ INT InSize)
{
    if (Header->Magic != UMFD_MAGIC || Header->Version != UMFD_VERSION ||
        Header->cbPayload != (ULONG)InSize - sizeof(UMFD_HEADER))
    {
        return FALSE;
    }
    return Header->RequestId != 0 || InSize == (INT)sizeof(UMFD_HEADER);
}

static INT
UmfdpServe(_In_ INT InSize, _In_opt_ LPSTR UnsafeIn, _In_ INT OutSize, _Out_opt_ LPSTR UnsafeOut)
{
    PEPROCESS Process = PsGetCurrentProcess();
    PUMFD_HEADER Incoming, Request = NULL;
    LARGE_INTEGER Timeout;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG cbRequest = 0;
    ULONG RequestId = 0;

    UMFD_HEADER Header;

    if (InSize < (INT)sizeof(UMFD_HEADER) || InSize > UMFD_MAX_MESSAGE || UnsafeIn == NULL ||
        OutSize < (INT)sizeof(UMFD_HEADER) || UnsafeOut == NULL)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Who is calling comes first: a non-host never makes win32k allocate or copy */
    ExAcquireFastMutex(&g_Umfd->Lock);
    if (g_Umfd->Host != Process || g_Umfd->ServeActive)
    {
        Status = (g_Umfd->Host != Process) ? STATUS_ACCESS_DENIED : STATUS_DEVICE_BUSY;
        ExReleaseFastMutex(&g_Umfd->Lock);
        SetLastNtError(Status);
        return 0;
    }
    g_Umfd->ServeActive = TRUE;
    ExReleaseFastMutex(&g_Umfd->Lock);

    _SEH2_TRY
    {
        ProbeForRead(UnsafeIn, InSize, 1);
        RtlCopyMemory(&Header, UnsafeIn, sizeof(Header));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    /* A SERVE without a reply is exactly one header */
    Incoming = NULL;
    if (NT_SUCCESS(Status) && !UmfdpIsValidHeader(&Header, InSize))
        Status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(Status) && Header.RequestId != 0)
    {
        Incoming = ExAllocatePoolWithTag(PagedPool, InSize, TAG_UMFD);
        if (Incoming == NULL)
            Status = STATUS_NO_MEMORY;
    }
    if (Incoming)
    {
        _SEH2_TRY
        {
            RtlCopyMemory(Incoming, UnsafeIn, InSize);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        /* The buffer is the host's to change between copies: check the captured one */
        if (NT_SUCCESS(Status) && (!UmfdpIsValidHeader(Incoming, InSize) || Incoming->RequestId == 0))
            Status = STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&g_Umfd->Lock);
    if (NT_SUCCESS(Status) && g_Umfd->Host != Process)
        Status = STATUS_PORT_DISCONNECTED;
    if (!NT_SUCCESS(Status))
    {
        g_Umfd->ServeActive = FALSE;
        ExReleaseFastMutex(&g_Umfd->Lock);
        if (Incoming)
            ExFreePoolWithTag(Incoming, TAG_UMFD);
        SetLastNtError(Status);
        return 0;
    }

    /* A reply counts only for the one outstanding request; a late or stale one is dropped */
    if (Incoming && g_Umfd->SlotState == UmfdSlotTaken &&
        Incoming->RequestId == g_Umfd->SlotRequestId && Incoming->Op == g_Umfd->SlotOp)
    {
        g_Umfd->Reply = Incoming;
        g_Umfd->SlotState = UmfdSlotDone;
        g_Umfd->SlotStatus = STATUS_SUCCESS;
        KeSetEvent(&g_Umfd->ReplyReady, IO_NO_INCREMENT, FALSE);
        Incoming = NULL;
    }
    ExReleaseFastMutex(&g_Umfd->Lock);
    if (Incoming)
        ExFreePoolWithTag(Incoming, TAG_UMFD);

    Timeout.QuadPart = -(LONGLONG)UMFD_SERVE_WAIT_MS * 10000;
    for (;;)
    {
        ExAcquireFastMutex(&g_Umfd->Lock);
        if (g_Umfd->Host != Process)
        {
            Status = STATUS_PORT_DISCONNECTED;
        }
        else if (g_Umfd->SlotState == UmfdSlotPosted)
        {
            Request = g_Umfd->Request;
            g_Umfd->Request = NULL;
            cbRequest = sizeof(UMFD_HEADER) + Request->cbPayload;
            RequestId = g_Umfd->SlotRequestId;
            if (cbRequest > (ULONG)OutSize)
            {
                g_Umfd->SlotState = UmfdSlotDone;
                g_Umfd->SlotStatus = STATUS_BUFFER_TOO_SMALL;
                KeSetEvent(&g_Umfd->ReplyReady, IO_NO_INCREMENT, FALSE);
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            else
            {
                g_Umfd->SlotState = UmfdSlotTaken;
                Status = STATUS_SUCCESS;
            }
        }
        else
        {
            Status = STATUS_PENDING;
        }
        if (Status != STATUS_PENDING)
            g_Umfd->ServeActive = FALSE;
        ExReleaseFastMutex(&g_Umfd->Lock);
        if (Status != STATUS_PENDING)
            break;

        /* UserMode and alertable: a dying or suspended host is never pinned in here */
        Status = KeWaitForSingleObject(&g_Umfd->RequestPosted, UserRequest, UserMode, TRUE, &Timeout);
        if (Status != STATUS_SUCCESS)
        {
            ExAcquireFastMutex(&g_Umfd->Lock);
            g_Umfd->ServeActive = FALSE;
            ExReleaseFastMutex(&g_Umfd->Lock);
            break;
        }
    }

    if (Request && NT_SUCCESS(Status))
    {
        _SEH2_TRY
        {
            ProbeForWrite(UnsafeOut, cbRequest, 1);
            RtlCopyMemory(UnsafeOut, Request, cbRequest);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        if (!NT_SUCCESS(Status))
        {
            /* Never delivered: fail that request instead of letting it time out */
            ExAcquireFastMutex(&g_Umfd->Lock);
            if (g_Umfd->SlotState == UmfdSlotTaken && g_Umfd->SlotRequestId == RequestId)
            {
                g_Umfd->SlotState = UmfdSlotDone;
                g_Umfd->SlotStatus = Status;
                KeSetEvent(&g_Umfd->ReplyReady, IO_NO_INCREMENT, FALSE);
            }
            ExReleaseFastMutex(&g_Umfd->Lock);
        }
    }
    if (Request)
        ExFreePoolWithTag(Request, TAG_UMFD);

    if (Status != STATUS_SUCCESS)
    {
        /* An idle timeout fails with WAIT_TIMEOUT: the host just calls SERVE again */
        if (Status == STATUS_TIMEOUT)
            EngSetLastError(WAIT_TIMEOUT);
        else
            SetLastNtError(Status);
        return 0;
    }
    return (INT)cbRequest;
}

INT
UmfdExtEscape(
    _In_opt_ HDC hDC,
    _In_opt_ PWCHAR pDriver,
    _In_ INT Escape,
    _In_ INT InSize,
    _In_opt_ LPSTR UnsafeIn,
    _In_ INT OutSize,
    _Out_opt_ LPSTR UnsafeOut)
{
    if (g_UmfdMode == 0)
    {
        EngSetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (hDC != NULL || pDriver != NULL)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    switch ((ULONG)Escape)
    {
        case UMFD_ESCAPE_REGISTER:
            return UmfdpRegister(InSize, UnsafeIn);
        case UMFD_ESCAPE_SERVE:
            return UmfdpServe(InSize, UnsafeIn, OutSize, UnsafeOut);
        case UMFD_ESCAPE_UNREGISTER:
            return UmfdpUnregister();
    }
    EngSetLastError(ERROR_INVALID_PARAMETER);
    return 0;
}

NTSTATUS
UmfdCallHost(
    _In_ USHORT Op,
    _In_ ULONG FontId,
    _In_reads_bytes_opt_(cbIn) const VOID *pIn,
    _In_ ULONG cbIn,
    _Outptr_result_maybenull_ PUMFD_HEADER *ppReply)
{
    PUMFD_HEADER Request, Reply = NULL;
    PEPROCESS Dead;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    *ppReply = NULL;
    if (g_UmfdMode == 0)
        return STATUS_NOT_SUPPORTED;
    if (cbIn > UMFD_MAX_MESSAGE - sizeof(UMFD_HEADER))
        return STATUS_INVALID_PARAMETER;

    /* A requester holding the USER lock would let a stuck host stall the session */
    ASSERT(!UserIsEntered());

    Request = ExAllocatePoolWithTag(PagedPool, sizeof(UMFD_HEADER) + cbIn, TAG_UMFD);
    if (Request == NULL)
        return STATUS_NO_MEMORY;
    Request->Magic = UMFD_MAGIC;
    Request->Version = UMFD_VERSION;
    Request->Op = Op;
    Request->RequestId = 0;
    Request->FontId = FontId;
    Request->Status = STATUS_SUCCESS;
    Request->cbPayload = cbIn;
    if (cbIn)
        RtlCopyMemory(Request + 1, pIn, cbIn);

    Timeout.QuadPart = -(LONGLONG)UMFD_REQUEST_TIMEOUT_MS * 10000;
    Status = KeWaitForSingleObject(&g_Umfd->SlotFree, Executive, KernelMode, FALSE, &Timeout);
    if (Status != STATUS_SUCCESS)
    {
        ExFreePoolWithTag(Request, TAG_UMFD);
        return STATUS_IO_TIMEOUT;
    }

    ExAcquireFastMutex(&g_Umfd->Lock);
    UmfdpDropDeadHostLocked(&Dead);
    if (g_Umfd->Host == NULL)
    {
        ExReleaseFastMutex(&g_Umfd->Lock);
        KeSetEvent(&g_Umfd->SlotFree, IO_NO_INCREMENT, FALSE);
        if (Dead)
            ObDereferenceObject(Dead);
        ExFreePoolWithTag(Request, TAG_UMFD);
        return STATUS_PORT_DISCONNECTED;
    }
    Request->RequestId = g_Umfd->NextRequestId++;
    if (g_Umfd->NextRequestId == 0)
        g_Umfd->NextRequestId = 1;
    g_Umfd->SlotRequestId = Request->RequestId;
    g_Umfd->SlotOp = Op;
    g_Umfd->SlotStatus = STATUS_PENDING;
    g_Umfd->Request = Request;
    g_Umfd->Reply = NULL;
    g_Umfd->SlotState = UmfdSlotPosted;
    KeClearEvent(&g_Umfd->ReplyReady);
    KeSetEvent(&g_Umfd->RequestPosted, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&g_Umfd->Lock);

    KeWaitForSingleObject(&g_Umfd->ReplyReady, Executive, KernelMode, FALSE, &Timeout);

    ExAcquireFastMutex(&g_Umfd->Lock);
    if (g_Umfd->SlotState == UmfdSlotDone)
    {
        Reply = g_Umfd->Reply;
        Status = g_Umfd->SlotStatus;
    }
    else
    {
        /* The id dies with the slot, so a late reply is dropped */
        Status = STATUS_IO_TIMEOUT;
    }
    Request = g_Umfd->Request;
    g_Umfd->Request = NULL;
    g_Umfd->Reply = NULL;
    g_Umfd->SlotRequestId = 0;
    g_Umfd->SlotState = UmfdSlotEmpty;
    ExReleaseFastMutex(&g_Umfd->Lock);
    KeSetEvent(&g_Umfd->SlotFree, IO_NO_INCREMENT, FALSE);

    if (Request)
        ExFreePoolWithTag(Request, TAG_UMFD);
    if (NT_SUCCESS(Status) && Reply)
    {
        *ppReply = Reply;
        return STATUS_SUCCESS;
    }
    if (Reply)
        ExFreePoolWithTag(Reply, TAG_UMFD);
    return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
}

VOID
UmfdFreeReply(_In_ PUMFD_HEADER Reply)
{
    ExFreePoolWithTag(Reply, TAG_UMFD);
}

VOID
UmfdProcessCleanup(_In_ PEPROCESS Process)
{
    PEPROCESS Host = NULL;

    /* Mode 0 never allocates the channel, so nothing can be registered */
    if (g_UmfdMode == 0)
        return;

    ExAcquireFastMutex(&g_Umfd->Lock);
    if (g_Umfd->Host == Process)
        Host = UmfdpChannelDownLocked();
    ExReleaseFastMutex(&g_Umfd->Lock);

    if (Host)
        ObDereferenceObject(Host);
}
