/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tramo D, Frente 1, lot 1: kernel32 exports forwarded to kernelbase
 */

#include "precomp.h"

static const char *const g_Forwarded[] =
{
    "GetThreadId", "GetProcessIdOfThread", "GetThreadPriorityBoost",
    "SetThreadPriorityBoost", "GetExitCodeThread", "GetThreadIOPendingFlag",
    "SuspendThread", "ResumeThread", "SwitchToThread", "OpenThread",
    "GetThreadTimes", "GetThreadIdealProcessorEx", "GetThreadGroupAffinity",
    "SetThreadIdealProcessor", "FlsAlloc", "FlsFree", "FlsGetValue",
    "FlsSetValue", "IsDebuggerPresent", "CheckRemoteDebuggerPresent",
};

static void test_ForwardsResolveToKernelBase(void)
{
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE hKB = GetModuleHandleW(L"kernelbase.dll");
    size_t i;

    ok(hK32 != NULL, "kernel32 not loaded\n");
    ok(hKB != NULL, "kernelbase not loaded\n");
    if (!hK32 || !hKB)
        return;

    for (i = 0; i < ARRAYSIZE(g_Forwarded); i++)
    {
        FARPROC p32 = GetProcAddress(hK32, g_Forwarded[i]);
        FARPROC pKB = GetProcAddress(hKB, g_Forwarded[i]);

        ok(p32 != NULL, "kernel32!%s not exported\n", g_Forwarded[i]);
        ok(pKB != NULL, "kernelbase!%s not exported\n", g_Forwarded[i]);
        /* a forwarder resolves to the target's address; a duplicate body would not */
        ok(p32 == pKB, "kernel32!%s (%p) is not the kernelbase body (%p)\n",
           g_Forwarded[i], p32, pKB);
    }
}

static void test_ThreadInfo(void)
{
    HANDLE hThread;
    DWORD exitCode, tid = GetCurrentThreadId();
    BOOL boost, pending;
    FILETIME c, e, k, u;

    ok(GetThreadId(GetCurrentThread()) == tid, "GetThreadId mismatch\n");
    ok(GetProcessIdOfThread(GetCurrentThread()) == GetCurrentProcessId(), "GetProcessIdOfThread mismatch\n");

    SetLastError(0xdeadbeef);
    ok(GetThreadId(NULL) == 0, "GetThreadId(NULL) should fail\n");
    ok(GetLastError() != 0xdeadbeef && GetLastError() != ERROR_SUCCESS, "no error set\n");

    ok(GetExitCodeThread(GetCurrentThread(), &exitCode) && exitCode == STILL_ACTIVE, "exit code %lu\n", exitCode);
    ok(GetThreadPriorityBoost(GetCurrentThread(), &boost), "GetThreadPriorityBoost failed %lu\n", GetLastError());
    ok(GetThreadIOPendingFlag(GetCurrentThread(), &pending), "GetThreadIOPendingFlag failed %lu\n", GetLastError());
    ok(!pending, "no I/O should be pending\n");
    ok(GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u), "GetThreadTimes failed %lu\n", GetLastError());

    hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    ok(hThread != NULL, "OpenThread failed %lu\n", GetLastError());
    if (hThread)
    {
        ok(GetThreadId(hThread) == tid, "GetThreadId(OpenThread) mismatch\n");
        CloseHandle(hThread);
    }
    SetLastError(0xdeadbeef);
    ok(OpenThread(THREAD_QUERY_INFORMATION, FALSE, 0xFFFFFFFC) == NULL, "OpenThread on bad id succeeded\n");
    ok(GetLastError() != 0xdeadbeef, "no error set\n");
}

static DWORD WINAPI WaitThread(LPVOID evt)
{
    WaitForSingleObject((HANDLE)evt, 5000);
    return 7;
}

static void test_SuspendResume(void)
{
    HANDLE hEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE hThread = CreateThread(NULL, 0, WaitThread, hEvt, CREATE_SUSPENDED, NULL);
    DWORD ret;

    ok(hThread != NULL, "CreateThread failed\n");
    if (!hThread)
        goto done;
    ret = SuspendThread(hThread);
    ok(ret == 1, "SuspendThread on created-suspended thread returned %lu\n", ret);
    ret = ResumeThread(hThread);
    ok(ret == 2, "first ResumeThread returned %lu\n", ret);
    ret = ResumeThread(hThread);
    ok(ret == 1, "second ResumeThread returned %lu\n", ret);
    SetEvent(hEvt);
    ok(WaitForSingleObject(hThread, 5000) == WAIT_OBJECT_0, "thread did not finish\n");
    ok(GetExitCodeThread(hThread, &ret) && ret == 7, "exit code %lu\n", ret);
    SetLastError(0xdeadbeef);
    ok(SuspendThread(NULL) == (DWORD)-1, "SuspendThread(NULL) should fail\n");
    ok(GetLastError() != 0xdeadbeef, "no error set\n");
    CloseHandle(hThread);
done:
    CloseHandle(hEvt);
}

static void test_Fls(void)
{
    DWORD idx = FlsAlloc(NULL);
    ok(idx != FLS_OUT_OF_INDEXES, "FlsAlloc failed %lu\n", GetLastError());
    if (idx == FLS_OUT_OF_INDEXES)
        return;
    ok(FlsSetValue(idx, (PVOID)(ULONG_PTR)0x1234), "FlsSetValue failed\n");
    SetLastError(0xdeadbeef);
    ok(FlsGetValue(idx) == (PVOID)(ULONG_PTR)0x1234, "FlsGetValue mismatch\n");
    ok(GetLastError() == ERROR_SUCCESS, "FlsGetValue must reset last error, got %lu\n", GetLastError());
    ok(FlsFree(idx), "FlsFree failed\n");
    SetLastError(0xdeadbeef);
    ok(!FlsFree(idx), "double FlsFree succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
}

static void test_Debugger(void)
{
    BOOL present = TRUE;

    ok((IsDebuggerPresent() != FALSE) == (NtCurrentTeb()->ProcessEnvironmentBlock->BeingDebugged != 0), "mismatch with PEB\n");
    ok(CheckRemoteDebuggerPresent(GetCurrentProcess(), &present), "CheckRemoteDebuggerPresent failed %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!CheckRemoteDebuggerPresent(NULL, &present), "NULL process succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!CheckRemoteDebuggerPresent(GetCurrentProcess(), NULL), "NULL out succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
}

START_TEST(InversionL1)
{
    test_ForwardsResolveToKernelBase();
    test_ThreadInfo();
    test_SuspendResume();
    test_Fls();
    test_Debugger();
}
