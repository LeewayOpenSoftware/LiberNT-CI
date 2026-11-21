/*
 * PROJECT:     ReactOS WinSock 2 API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Extended Protocol-Independent Address Resolution
 * COPYRIGHT:   Adapted from Wine dlls/ws2_32/protocol.c
 */

#include <ws2_32.h>

#include <ws2tcpip.h>

#define NDEBUG
#include <debug.h>

static
INT
WSAAPI
ConvertAddrInfoToAddrInfoEx(IN PADDRINFOW AddrInfo,
                            OUT PADDRINFOEXW *ppResult)
{
    PADDRINFOEXW New;
    PADDRINFOEXW *Next = ppResult;
    SIZE_T NameSize;

    *ppResult = NULL;

    for (; AddrInfo; AddrInfo = AddrInfo->ai_next)
    {
        New = HeapAlloc(WsSockHeap, HEAP_ZERO_MEMORY, sizeof(ADDRINFOEXW));
        if (!New) goto Failure;

        *Next = New;

        New->ai_flags = AddrInfo->ai_flags;
        New->ai_family = AddrInfo->ai_family;
        New->ai_socktype = AddrInfo->ai_socktype;
        New->ai_protocol = AddrInfo->ai_protocol;

        if (AddrInfo->ai_addr)
        {
            New->ai_addr = HeapAlloc(WsSockHeap, 0, AddrInfo->ai_addrlen);
            if (!New->ai_addr) goto Failure;

            New->ai_addrlen = AddrInfo->ai_addrlen;
            RtlMoveMemory(New->ai_addr, AddrInfo->ai_addr, AddrInfo->ai_addrlen);
        }

        if (AddrInfo->ai_canonname)
        {
            NameSize = (wcslen(AddrInfo->ai_canonname) + 1) * sizeof(WCHAR);
            New->ai_canonname = HeapAlloc(WsSockHeap, 0, NameSize);
            if (!New->ai_canonname) goto Failure;

            RtlMoveMemory(New->ai_canonname, AddrInfo->ai_canonname, NameSize);
        }

        Next = &New->ai_next;
    }

    return ERROR_SUCCESS;

Failure:
    FreeAddrInfoExW(*ppResult);
    *ppResult = NULL;
    return WSA_NOT_ENOUGH_MEMORY;
}

static
INT
WSAAPI
GetAddrInfoExSync(IN PCWSTR pName,
                  IN PCWSTR pServiceName,
                  IN PADDRINFOW Hints,
                  OUT PADDRINFOEXW *ppResult)
{
    PADDRINFOW Result = NULL;
    INT ErrorCode;

    *ppResult = NULL;

    ErrorCode = GetAddrInfoW(pName, pServiceName, Hints, &Result);
    if (ErrorCode != ERROR_SUCCESS) return ErrorCode;

    ErrorCode = ConvertAddrInfoToAddrInfoEx(Result, ppResult);

    freeaddrinfo((LPADDRINFO)Result);

    return ErrorCode;
}

typedef struct _WS_ADDRINFOEX_REQUEST
{
    LIST_ENTRY ListEntry;
    LONG RefCount;
    LONG State;
    PWSTR Name;
    PWSTR ServiceName;
    BOOL HaveHints;
    ADDRINFOW Hints;
    PADDRINFOEXW *ppResult;
    LPOVERLAPPED Overlapped;
    LPLOOKUPSERVICE_COMPLETION_ROUTINE CompletionRoutine;
    INT Error;
} WS_ADDRINFOEX_REQUEST, *PWS_ADDRINFOEX_REQUEST;

#define WS_ADDRINFOEX_PENDING 0
#define WS_ADDRINFOEX_DONE    1

/* Requests that may still be cancelled; a handle is valid only while it is listed here */
static LIST_ENTRY WsAddrInfoExPending = { &WsAddrInfoExPending, &WsAddrInfoExPending };
static LONG WsAddrInfoExLock;

static
VOID
WsAddrInfoExAcquire(VOID)
{
    while (InterlockedCompareExchange(&WsAddrInfoExLock, 1, 0) != 0)
        SwitchToThread();
}

static
VOID
WsAddrInfoExRelease(VOID)
{
    InterlockedExchange(&WsAddrInfoExLock, 0);
}

static
PWSTR
WsAddrInfoExDupString(IN PCWSTR String,
                      OUT PBOOL Failed)
{
    SIZE_T Size;
    PWSTR Copy;

    if (!String) return NULL;

    Size = (wcslen(String) + 1) * sizeof(WCHAR);
    Copy = HeapAlloc(WsSockHeap, 0, Size);
    if (Copy)
        RtlMoveMemory(Copy, String, Size);
    else
        *Failed = TRUE;

    return Copy;
}

static
VOID
WsAddrInfoExDereference(IN PWS_ADDRINFOEX_REQUEST Request)
{
    if (InterlockedDecrement(&Request->RefCount) != 0) return;

    if (Request->Name) HeapFree(WsSockHeap, 0, Request->Name);
    if (Request->ServiceName) HeapFree(WsSockHeap, 0, Request->ServiceName);
    HeapFree(WsSockHeap, 0, Request);
}

/* Called once per request, by whichever side won the PENDING -> DONE transition */
static
VOID
WsAddrInfoExDeliver(IN PWS_ADDRINFOEX_REQUEST Request)
{
    LPOVERLAPPED Overlapped = Request->Overlapped;

    Overlapped->InternalHigh = 0;
    Overlapped->Internal = (ULONG_PTR)(LONG_PTR)Request->Error;

    if (Request->CompletionRoutine)
        Request->CompletionRoutine(Request->Error, 0, Overlapped);
    else if (Overlapped->hEvent)
        SetEvent(Overlapped->hEvent);

    WsAddrInfoExDereference(Request);
}

static
BOOL
WsAddrInfoExClaim(IN PWS_ADDRINFOEX_REQUEST Request)
{
    BOOL Won = FALSE;

    WsAddrInfoExAcquire();
    if (Request->State == WS_ADDRINFOEX_PENDING)
    {
        Request->State = WS_ADDRINFOEX_DONE;
        RemoveEntryList(&Request->ListEntry);
        Won = TRUE;
    }
    WsAddrInfoExRelease();

    return Won;
}

static
DWORD
WINAPI
WsAddrInfoExCancelDelivery(IN PVOID Context)
{
    WsAddrInfoExDeliver(Context);
    return 0;
}

static
DWORD
WINAPI
WsAddrInfoExWorker(IN PVOID Context)
{
    PWS_ADDRINFOEX_REQUEST Request = Context;
    PADDRINFOEXW Result = NULL;
    INT ErrorCode;

    ErrorCode = GetAddrInfoExSync(Request->Name,
                                  Request->ServiceName,
                                  Request->HaveHints ? &Request->Hints : NULL,
                                  &Result);

    if (WsAddrInfoExClaim(Request))
    {
        *Request->ppResult = Result;
        Request->Error = ErrorCode;
        WsAddrInfoExDeliver(Request);
    }
    else
    {
        /* Cancelled: the canceller delivered WSA_E_CANCELLED and owns the delivery reference */
        if (Result) FreeAddrInfoExW(Result);
    }

    WsAddrInfoExDereference(Request);
    return 0;
}

static
INT
WSAAPI
GetAddrInfoExAsync(IN PCWSTR pName,
                   IN PCWSTR pServiceName,
                   IN PADDRINFOW Hints,
                   OUT PADDRINFOEXW *ppResult,
                   IN LPOVERLAPPED lpOverlapped,
                   IN LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine,
                   OUT LPHANDLE lpHandle)
{
    PWS_ADDRINFOEX_REQUEST Request;
    BOOL Failed = FALSE;

    Request = HeapAlloc(WsSockHeap, HEAP_ZERO_MEMORY, sizeof(*Request));
    if (!Request) return WSA_NOT_ENOUGH_MEMORY;

    Request->Name = WsAddrInfoExDupString(pName, &Failed);
    Request->ServiceName = WsAddrInfoExDupString(pServiceName, &Failed);
    if (Failed)
    {
        Request->RefCount = 1;
        WsAddrInfoExDereference(Request);
        return WSA_NOT_ENOUGH_MEMORY;
    }

    if (Hints)
    {
        Request->Hints = *Hints;
        Request->HaveHints = TRUE;
    }
    Request->ppResult = ppResult;
    Request->Overlapped = lpOverlapped;
    Request->CompletionRoutine = lpCompletionRoutine;
    Request->State = WS_ADDRINFOEX_PENDING;
    /* One reference for the worker, one for the delivery */
    Request->RefCount = 2;

    lpOverlapped->Internal = WSAEINPROGRESS;
    lpOverlapped->InternalHigh = 0;

    WsAddrInfoExAcquire();
    InsertTailList(&WsAddrInfoExPending, &Request->ListEntry);
    WsAddrInfoExRelease();

    if (lpHandle) *lpHandle = (HANDLE)Request;

    if (!QueueUserWorkItem(WsAddrInfoExWorker, Request, WT_EXECUTELONGFUNCTION))
    {
        /* A cancel may already own the delivery; then the request still completes */
        if (!WsAddrInfoExClaim(Request))
        {
            WsAddrInfoExDereference(Request);
            return WSA_IO_PENDING;
        }
        if (lpHandle) *lpHandle = NULL;
        lpOverlapped->Internal = WSA_NOT_ENOUGH_MEMORY;
        WsAddrInfoExDereference(Request);
        WsAddrInfoExDereference(Request);
        return WSA_NOT_ENOUGH_MEMORY;
    }

    return WSA_IO_PENDING;
}

INT
WSAAPI
GetAddrInfoExW(IN PCWSTR pName,
               IN PCWSTR pServiceName,
               IN DWORD dwNameSpace,
               IN LPGUID lpNspId,
               IN const ADDRINFOEXW *hints,
               OUT PADDRINFOEXW *ppResult,
               IN struct timeval *timeout,
               IN LPOVERLAPPED lpOverlapped,
               IN LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine,
               OUT LPHANDLE lpHandle)
{
    INT ErrorCode;
    ADDRINFOW Hints;
    PADDRINFOW HintsW = NULL;

    DPRINT("GetAddrInfoExW: %S, %S, %lu, %p, %p\n", pName, pServiceName, dwNameSpace, ppResult, lpOverlapped);

    if (lpHandle) *lpHandle = NULL;

    if ((ErrorCode = WsQuickProlog()) != ERROR_SUCCESS)
    {
        SetLastError(ErrorCode);
        return ErrorCode;
    }

    if (!ppResult)
    {
        SetLastError(WSAEFAULT);
        return WSAEFAULT;
    }

    *ppResult = NULL;

    /* A completion routine is only meaningful for an overlapped request */
    if ((lpCompletionRoutine) && !(lpOverlapped))
    {
        SetLastError(WSAEINVAL);
        return WSAEINVAL;
    }

    if (((dwNameSpace != NS_ALL) && (dwNameSpace != NS_DNS)) || (lpNspId))
    {
        SetLastError(WSAEINVAL);
        return WSAEINVAL;
    }

    if (!(pName) && !(pServiceName))
    {
        SetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }

    if (hints)
    {
        RtlZeroMemory(&Hints, sizeof(Hints));
        Hints.ai_flags = hints->ai_flags;
        Hints.ai_family = hints->ai_family;
        Hints.ai_socktype = hints->ai_socktype;
        Hints.ai_protocol = hints->ai_protocol;
        HintsW = &Hints;
    }

    if (lpOverlapped)
    {
        ErrorCode = GetAddrInfoExAsync(pName,
                                       pServiceName,
                                       HintsW,
                                       ppResult,
                                       lpOverlapped,
                                       lpCompletionRoutine,
                                       lpHandle);
    }
    else
    {
        ErrorCode = GetAddrInfoExSync(pName, pServiceName, HintsW, ppResult);
    }

    SetLastError(ErrorCode);
    return ErrorCode;
}

VOID
WSAAPI
FreeAddrInfoExW(IN PADDRINFOEXW pAddrInfoEx)
{
    PADDRINFOEXW NextInfo;

    for (NextInfo = pAddrInfoEx; NextInfo; NextInfo = pAddrInfoEx)
    {
        if (NextInfo->ai_canonname)
        {
            HeapFree(WsSockHeap, 0, NextInfo->ai_canonname);
        }

        if (NextInfo->ai_addr)
        {
            HeapFree(WsSockHeap, 0, NextInfo->ai_addr);
        }

        pAddrInfoEx = NextInfo->ai_next;

        HeapFree(WsSockHeap, 0, NextInfo);
    }
}

INT
WSAAPI
GetAddrInfoExCancel(IN LPHANDLE lpHandle)
{
    PWS_ADDRINFOEX_REQUEST Request = NULL;
    PLIST_ENTRY Entry;
    HANDLE Handle;

    DPRINT("GetAddrInfoExCancel: %p\n", lpHandle);

    _SEH2_TRY
    {
        Handle = lpHandle ? *lpHandle : NULL;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Handle = NULL;
    }
    _SEH2_END;

    if (Handle)
    {
        WsAddrInfoExAcquire();
        for (Entry = WsAddrInfoExPending.Flink; Entry != &WsAddrInfoExPending; Entry = Entry->Flink)
        {
            if ((HANDLE)CONTAINING_RECORD(Entry, WS_ADDRINFOEX_REQUEST, ListEntry) != Handle) continue;

            Request = (PWS_ADDRINFOEX_REQUEST)Handle;
            Request->State = WS_ADDRINFOEX_DONE;
            Request->Error = WSA_E_CANCELLED;
            RemoveEntryList(&Request->ListEntry);
            break;
        }
        WsAddrInfoExRelease();
    }

    if (!Request)
    {
        SetLastError(WSA_INVALID_HANDLE);
        return WSA_INVALID_HANDLE;
    }

    /* Complete from a pool thread, as a normal completion would; inline only if that fails */
    if (!QueueUserWorkItem(WsAddrInfoExCancelDelivery, Request, WT_EXECUTEDEFAULT))
        WsAddrInfoExDeliver(Request);

    return NO_ERROR;
}

INT
WSAAPI
GetAddrInfoExOverlappedResult(IN LPOVERLAPPED lpOverlapped)
{
    DPRINT("GetAddrInfoExOverlappedResult: %p\n", lpOverlapped);

    if (!lpOverlapped) return WSAEINVAL;

    return (INT)(LONG_PTR)lpOverlapped->Internal;
}
