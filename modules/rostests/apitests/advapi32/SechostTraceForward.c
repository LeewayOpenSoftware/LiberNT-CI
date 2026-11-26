/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     sechost trace entry points resolve to the advapi32 implementations
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"
#include <evntrace.h>

static HMODULE g_Sechost;

static const GUID TestGuid = {0x8e9f5a12, 0x3b7c, 0x4d1e, {0x9a, 0x2f, 0x5c, 0x6b, 0x8d, 0x0e, 0x1f, 0x42}};

typedef ULONG (WINAPI *ENABLE_TRACE_EX2_FN)(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, PENABLE_TRACE_PARAMETERS);
typedef TRACEHANDLE (WINAPI *OPEN_TRACE_W_FN)(PEVENT_TRACE_LOGFILEW);
typedef ULONG (WINAPI *PROCESS_TRACE_FN)(PTRACEHANDLE, ULONG, LPFILETIME, LPFILETIME);
typedef ULONG (WINAPI *CLOSE_TRACE_FN)(TRACEHANDLE);
typedef ULONG (WINAPI *TRACE_SET_INFORMATION_FN)(TRACEHANDLE, TRACE_INFO_CLASS, PVOID, ULONG);
typedef ULONG (WINAPI *SET_TRACE_CALLBACK_FN)(LPCGUID, PEVENT_CALLBACK);
typedef ULONG (WINAPI *REMOVE_TRACE_CALLBACK_FN)(LPCGUID);

static VOID TestEnableTraceEx2(VOID)
{
    ENABLE_TRACE_EX2_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (ENABLE_TRACE_EX2_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "EnableTraceEx2");
    SecHost = (ENABLE_TRACE_EX2_FN)GetProcAddress(g_Sechost, "EnableTraceEx2");
    ok(AdvApi != NULL && SecHost != NULL, "EnableTraceEx2 resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(0, &TestGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, 0, 0, 0, 0, NULL);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(0, &TestGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, 0, 0, 0, 0, NULL);
    ok(Ra == Rs, "EnableTraceEx2 result adv=%lu sec=%lu\n", Ra, Rs);
}

static VOID TestOpenTraceW(VOID)
{
    OPEN_TRACE_W_FN AdvApi, SecHost;
    EVENT_TRACE_LOGFILEW Logfile;
    TRACEHANDLE Ha, Hs;

    AdvApi = (OPEN_TRACE_W_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "OpenTraceW");
    SecHost = (OPEN_TRACE_W_FN)GetProcAddress(g_Sechost, "OpenTraceW");
    ok(AdvApi != NULL && SecHost != NULL, "OpenTraceW resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    ZeroMemory(&Logfile, sizeof(Logfile));

    SetLastError(0xDEADBEEF);
    Ha = AdvApi(&Logfile);
    SetLastError(0xDEADBEEF);
    Hs = SecHost(&Logfile);
    ok(Ha == Hs && Ha == INVALID_PROCESSTRACE_HANDLE,
       "OpenTraceW handle adv=%I64x sec=%I64x\n", Ha, Hs);
    ok(GetLastError() == 0xDEADBEEF || GetLastError() == ERROR_SUCCESS,
       "OpenTraceW last error %lu\n", GetLastError());
}

static VOID TestProcessTrace(VOID)
{
    PROCESS_TRACE_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (PROCESS_TRACE_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "ProcessTrace");
    SecHost = (PROCESS_TRACE_FN)GetProcAddress(g_Sechost, "ProcessTrace");
    ok(AdvApi != NULL && SecHost != NULL, "ProcessTrace resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(NULL, 0, NULL, NULL);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(NULL, 0, NULL, NULL);
    ok(Ra == Rs, "ProcessTrace result adv=%lu sec=%lu\n", Ra, Rs);
}

static VOID TestCloseTrace(VOID)
{
    CLOSE_TRACE_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (CLOSE_TRACE_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "CloseTrace");
    SecHost = (CLOSE_TRACE_FN)GetProcAddress(g_Sechost, "CloseTrace");
    ok(AdvApi != NULL && SecHost != NULL, "CloseTrace resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(0);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(0);
    ok(Ra == Rs, "CloseTrace result adv=%lu sec=%lu\n", Ra, Rs);
}

static VOID TestTraceSetInformation(VOID)
{
    TRACE_SET_INFORMATION_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (TRACE_SET_INFORMATION_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "TraceSetInformation");
    SecHost = (TRACE_SET_INFORMATION_FN)GetProcAddress(g_Sechost, "TraceSetInformation");
    ok(AdvApi != NULL && SecHost != NULL, "TraceSetInformation resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(0, TraceStackTracingInfo, NULL, 0);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(0, TraceStackTracingInfo, NULL, 0);
    ok(Ra == Rs, "TraceSetInformation result adv=%lu sec=%lu\n", Ra, Rs);
    ok(Ra == ERROR_NOT_SUPPORTED, "TraceSetInformation returned %lu\n", Ra);
}

static VOID TestSetTraceCallback(VOID)
{
    SET_TRACE_CALLBACK_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (SET_TRACE_CALLBACK_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "SetTraceCallback");
    SecHost = (SET_TRACE_CALLBACK_FN)GetProcAddress(g_Sechost, "SetTraceCallback");
    ok(AdvApi != NULL && SecHost != NULL, "SetTraceCallback resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(&TestGuid, NULL);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(&TestGuid, NULL);
    ok(Ra == Rs, "SetTraceCallback result adv=%lu sec=%lu\n", Ra, Rs);
    ok(Ra == ERROR_NOT_SUPPORTED, "SetTraceCallback returned %lu\n", Ra);
}

static VOID TestRemoveTraceCallback(VOID)
{
    REMOVE_TRACE_CALLBACK_FN AdvApi, SecHost;
    ULONG Ra, Rs;

    AdvApi = (REMOVE_TRACE_CALLBACK_FN)GetProcAddress(GetModuleHandleW(L"advapi32"), "RemoveTraceCallback");
    SecHost = (REMOVE_TRACE_CALLBACK_FN)GetProcAddress(g_Sechost, "RemoveTraceCallback");
    ok(AdvApi != NULL && SecHost != NULL, "RemoveTraceCallback resolve adv=%p sec=%p\n", AdvApi, SecHost);
    if (!AdvApi || !SecHost)
        return;

    SetLastError(0xDEADBEEF);
    Ra = AdvApi(&TestGuid);
    SetLastError(0xDEADBEEF);
    Rs = SecHost(&TestGuid);
    ok(Ra == Rs, "RemoveTraceCallback result adv=%lu sec=%lu\n", Ra, Rs);
    ok(Ra == ERROR_NOT_SUPPORTED, "RemoveTraceCallback returned %lu\n", Ra);
}

START_TEST(SechostTraceForward)
{
    g_Sechost = LoadLibraryW(L"sechost.dll");
    ok(g_Sechost != NULL, "LoadLibraryW(sechost) failed: %lu\n", GetLastError());
    if (!g_Sechost)
        return;

    TestEnableTraceEx2();
    TestOpenTraceW();
    TestProcessTrace();
    TestCloseTrace();
    TestTraceSetInformation();
    TestSetTraceCallback();
    TestRemoveTraceCallback();

    FreeLibrary(g_Sechost);
}
