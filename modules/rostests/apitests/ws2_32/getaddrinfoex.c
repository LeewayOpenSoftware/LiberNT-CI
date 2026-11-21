/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetAddrInfoExW (synchronous and overlapped), GetAddrInfoExCancel
 *              and GetAddrInfoExOverlappedResult
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"

/* Local copies so the test builds whatever _WIN32_WINNT the apitests use */
typedef struct _TEST_ADDRINFOEXW
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    PWSTR ai_canonname;
    struct sockaddr *ai_addr;
    void *ai_blob;
    size_t ai_bloblen;
    LPGUID ai_provider;
    struct _TEST_ADDRINFOEXW *ai_next;
} TEST_ADDRINFOEXW, *PTEST_ADDRINFOEXW;

typedef VOID (CALLBACK *PTEST_COMPLETION)(DWORD dwError, DWORD dwBytes, LPWSAOVERLAPPED lpOverlapped);
typedef INT (WSAAPI *PGETADDRINFOEXW)(PCWSTR, PCWSTR, DWORD, LPGUID, const TEST_ADDRINFOEXW *,
                                      PTEST_ADDRINFOEXW *, struct timeval *, LPOVERLAPPED,
                                      PTEST_COMPLETION, LPHANDLE);
typedef VOID (WSAAPI *PFREEADDRINFOEXW)(PTEST_ADDRINFOEXW);
typedef INT (WSAAPI *PGETADDRINFOEXCANCEL)(LPHANDLE);
typedef INT (WSAAPI *PGETADDRINFOEXOVERLAPPEDRESULT)(LPOVERLAPPED);

static PGETADDRINFOEXW pGetAddrInfoExW;
static PFREEADDRINFOEXW pFreeAddrInfoExW;
static PGETADDRINFOEXCANCEL pGetAddrInfoExCancel;
static PGETADDRINFOEXOVERLAPPEDRESULT pGetAddrInfoExOverlappedResult;

#ifndef NS_DNS
#define NS_DNS 12
#endif
#ifndef WSA_E_CANCELLED
#define WSA_E_CANCELLED 10111
#endif

static HANDLE CompletionEvent;
static volatile LONG CompletionCount;
static volatile DWORD CompletionError;
static volatile DWORD CompletionBytes;
static LPWSAOVERLAPPED volatile CompletionOverlapped;
static volatile DWORD CompletionThread;

static
VOID
CALLBACK
Completion(DWORD dwError, DWORD dwBytes, LPWSAOVERLAPPED lpOverlapped)
{
    CompletionError = dwError;
    CompletionBytes = dwBytes;
    CompletionOverlapped = lpOverlapped;
    CompletionThread = GetCurrentThreadId();
    InterlockedIncrement(&CompletionCount);
    SetEvent(CompletionEvent);
}

static
VOID
ResetCompletion(VOID)
{
    CompletionCount = 0;
    CompletionError = 0xdeadbeef;
    CompletionBytes = 0xdeadbeef;
    CompletionOverlapped = NULL;
    CompletionThread = 0;
    ResetEvent(CompletionEvent);
}

static
VOID
CheckLoopbackResult(PTEST_ADDRINFOEXW Result)
{
    ok(Result != NULL, "no result\n");
    if (!Result) return;
    ok_dec(Result->ai_family, AF_INET);
    ok(Result->ai_addr != NULL, "no address\n");
    if (Result->ai_addr)
    {
        ok_dec(((SOCKADDR_IN *)Result->ai_addr)->sin_family, AF_INET);
        ok_hex(((SOCKADDR_IN *)Result->ai_addr)->sin_addr.S_un.S_addr, inet_addr("127.0.0.1"));
        ok_dec(ntohs(((SOCKADDR_IN *)Result->ai_addr)->sin_port), 80);
    }
}

static
VOID
TestSync(VOID)
{
    PTEST_ADDRINFOEXW Result = (PVOID)0x1234;
    HANDLE Handle = (HANDLE)0x5678;
    INT Error;

    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, &Result, NULL, NULL, NULL, &Handle);
    ok_dec(Error, 0);
    ok(Handle == NULL, "Handle = %p\n", Handle);
    CheckLoopbackResult(Result);
    if (Error == 0 && Result) pFreeAddrInfoExW(Result);
}

static
VOID
TestEvent(VOID)
{
    PTEST_ADDRINFOEXW Result = (PVOID)0x1234;
    OVERLAPPED Overlapped;
    HANDLE Handle = NULL;
    INT Error;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, &Result, NULL, &Overlapped, NULL, &Handle);
    ok(Error == WSA_IO_PENDING || Error == 0, "Error = %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        ok(Handle != NULL, "no cancel handle\n");
        ok_dec(WaitForSingleObject(Overlapped.hEvent, 10000), WAIT_OBJECT_0);
    }
    ok_dec(pGetAddrInfoExOverlappedResult(&Overlapped), 0);
    ok_dec((DWORD)Overlapped.InternalHigh, 0);
    CheckLoopbackResult(Result);
    if (Result && Result != (PVOID)0x1234) pFreeAddrInfoExW(Result);

    /* a completed handle is no longer cancellable */
    if (Handle)
        ok_dec(pGetAddrInfoExCancel(&Handle), WSA_INVALID_HANDLE);

    CloseHandle(Overlapped.hEvent);
}

static
VOID
TestRoutine(VOID)
{
    PTEST_ADDRINFOEXW Result = (PVOID)0x1234;
    OVERLAPPED Overlapped;
    HANDLE Handle = NULL;
    INT Error;

    ResetCompletion();
    ZeroMemory(&Overlapped, sizeof(Overlapped));

    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, &Result, NULL, &Overlapped, Completion, &Handle);
    ok(Error == WSA_IO_PENDING || Error == 0, "Error = %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        ok_dec(WaitForSingleObject(CompletionEvent, 10000), WAIT_OBJECT_0);
        ok_dec(CompletionError, 0);
        ok_dec(CompletionBytes, 0);
        ok(CompletionOverlapped == (LPWSAOVERLAPPED)&Overlapped, "lpOverlapped = %p\n", CompletionOverlapped);
        ok(CompletionThread != GetCurrentThreadId(), "routine ran on the calling thread\n");
        ok_dec(pGetAddrInfoExOverlappedResult(&Overlapped), 0);
    }
    CheckLoopbackResult(Result);
    if (Result && Result != (PVOID)0x1234) pFreeAddrInfoExW(Result);

    Sleep(100);
    ok_dec(CompletionCount, (Error == WSA_IO_PENDING) ? 1 : 0);
}

/* Cancel races the completion; whichever wins, exactly one completion must arrive and match */
static
VOID
TestCancelRace(VOID)
{
    PTEST_ADDRINFOEXW Result;
    OVERLAPPED Overlapped;
    HANDLE Handle;
    INT Error, CancelError, i;
    INT Cancelled = 0, Completed = 0;

    for (i = 0; i < 50; i++)
    {
        ResetCompletion();
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Result = NULL;
        Handle = NULL;

        Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, &Result, NULL, &Overlapped, Completion, &Handle);
        if (Error != WSA_IO_PENDING)
        {
            ok_dec(Error, 0);
            if (Result) pFreeAddrInfoExW(Result);
            continue;
        }

        CancelError = pGetAddrInfoExCancel(&Handle);
        ok(CancelError == NO_ERROR || CancelError == WSA_INVALID_HANDLE, "Cancel = %d\n", CancelError);
        ok_dec(WaitForSingleObject(CompletionEvent, 10000), WAIT_OBJECT_0);
        Sleep(20);
        ok_dec(CompletionCount, 1);

        if (CompletionError == WSA_E_CANCELLED)
        {
            ok_dec(CancelError, NO_ERROR);
            ok(Result == NULL, "cancelled request returned %p\n", Result);
            ok_dec(pGetAddrInfoExOverlappedResult(&Overlapped), WSA_E_CANCELLED);
            Cancelled++;
        }
        else
        {
            ok_dec(CompletionError, 0);
            ok_dec(CancelError, WSA_INVALID_HANDLE);
            CheckLoopbackResult(Result);
            Completed++;
        }
        if (Result) pFreeAddrInfoExW(Result);
    }

    trace("cancel race: %d cancelled, %d completed first\n", Cancelled, Completed);
}

static
VOID
TestInvalid(VOID)
{
    PTEST_ADDRINFOEXW Result = NULL;
    OVERLAPPED Overlapped;
    HANDLE Bogus = (HANDLE)&Overlapped;
    HANDLE Null = NULL;
    INT Error;

    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    ok_dec(Error, WSAEFAULT);

    ResetCompletion();
    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, NULL, &Result, NULL, NULL, Completion, NULL);
    ok_dec(Error, WSAEINVAL);
    ok(Result == NULL, "Result = %p\n", Result);
    Sleep(50);
    ok_dec(CompletionCount, 0);

    ok_dec(pGetAddrInfoExCancel(NULL), WSA_INVALID_HANDLE);
    ok_dec(pGetAddrInfoExCancel(&Null), WSA_INVALID_HANDLE);
    ok_dec(pGetAddrInfoExCancel(&Bogus), WSA_INVALID_HANDLE);

    ok_dec(pGetAddrInfoExOverlappedResult(NULL), WSAEINVAL);
}

START_TEST(getaddrinfoex)
{
    WSADATA WsaData;
    HMODULE Ws2;

    ok_dec(WSAStartup(MAKEWORD(2, 2), &WsaData), 0);

    Ws2 = GetModuleHandleW(L"ws2_32.dll");
    pGetAddrInfoExW = (PGETADDRINFOEXW)GetProcAddress(Ws2, "GetAddrInfoExW");
    pFreeAddrInfoExW = (PFREEADDRINFOEXW)GetProcAddress(Ws2, "FreeAddrInfoExW");
    pGetAddrInfoExCancel = (PGETADDRINFOEXCANCEL)GetProcAddress(Ws2, "GetAddrInfoExCancel");
    pGetAddrInfoExOverlappedResult = (PGETADDRINFOEXOVERLAPPEDRESULT)GetProcAddress(Ws2, "GetAddrInfoExOverlappedResult");
    if (!pGetAddrInfoExW || !pFreeAddrInfoExW || !pGetAddrInfoExCancel || !pGetAddrInfoExOverlappedResult)
    {
        skip("GetAddrInfoEx family not exported (%p %p %p %p)\n", pGetAddrInfoExW, pFreeAddrInfoExW,
             pGetAddrInfoExCancel, pGetAddrInfoExOverlappedResult);
        WSACleanup();
        return;
    }

    CompletionEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    TestSync();
    TestEvent();
    TestRoutine();
    TestCancelRace();
    TestInvalid();

    CloseHandle(CompletionEvent);
    WSACleanup();
}
