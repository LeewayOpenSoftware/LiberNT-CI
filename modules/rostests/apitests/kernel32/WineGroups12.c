/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Baseline for the volume/sync/memory groups of kernelbase (Tramo D, G1-G2)
 *
 * These tests pin the documented behavior of exports whose body also exists in
 * the not-yet-linked Wine sources (wine/volume.c, sync.c, memory.c). They must
 * pass before and after any export is switched to the Wine body.
 */

#include "precomp.h"

static HMODULE g_hKernelBase;

static FARPROC GetKernelBaseProc(const char *name)
{
    if (!g_hKernelBase)
        g_hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    return g_hKernelBase ? GetProcAddress(g_hKernelBase, name) : NULL;
}

static void test_GetLogicalDriveStrings(void)
{
    WCHAR buf[512];
    DWORD need, got, drives, i, count = 0;

    drives = GetLogicalDrives();
    ok(drives != 0, "GetLogicalDrives returned 0\n");
    for (i = 0; i < 26; i++)
        if (drives & (1u << i))
            count++;

    /* each drive is "X:\\" + NUL, plus the final NUL */
    need = GetLogicalDriveStringsW(0, NULL);
    ok(need == count * 4 || need == count * 4 + 1,
       "size query returned %lu for %lu drives\n", need, count);

    /* too small: returns the size required, buffer untouched is not promised */
    got = GetLogicalDriveStringsW(1, buf);
    ok(got == need, "short buffer returned %lu, expected %lu\n", got, need);

    /* enough room: returns the length without the final NUL */
    ok(need < ARRAYSIZE(buf), "test buffer too small\n");
    got = GetLogicalDriveStringsW(ARRAYSIZE(buf), buf);
    ok(got == count * 4, "full call returned %lu, expected %lu\n", got, count * 4);
    ok(buf[got] == 0, "missing double NUL terminator\n");
    if (got >= 3)
    {
        ok(buf[1] == L':' && buf[2] == L'\\' && buf[3] == 0,
           "unexpected first entry %S\n", buf);
    }
}

static void test_DriveTypeInvalid(void)
{
    ok(GetDriveTypeW(L"\\\\?\\NoSuchDevice\\") != DRIVE_FIXED, "invented a fixed drive\n");
}

static void test_ConditionVariableTimeout(void)
{
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    BOOL ret;

    InitializeSRWLock(&lock);
    InitializeConditionVariable(&cv);

    AcquireSRWLockExclusive(&lock);
    SetLastError(0xdeadbeef);
    ret = SleepConditionVariableSRW(&cv, &lock, 10, 0);
    ok(!ret, "SleepConditionVariableSRW should time out\n");
    ok(GetLastError() == ERROR_TIMEOUT, "got error %lu\n", GetLastError());
    ReleaseSRWLockExclusive(&lock);
}

static void test_CreateEventExW(void)
{
    HANDLE h, h2;
    DWORD ret;

    h = CreateEventExW(NULL, L"WineG12Event", CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    ok(h != NULL, "CreateEventExW failed %lu\n", GetLastError());
    if (!h)
        return;
    ok(GetLastError() == 0 || GetLastError() == ERROR_SUCCESS, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    h2 = CreateEventExW(NULL, L"WineG12Event", 0, EVENT_ALL_ACCESS);
    ok(h2 != NULL, "second CreateEventExW failed %lu\n", GetLastError());
    ok(GetLastError() == ERROR_ALREADY_EXISTS, "got error %lu\n", GetLastError());
    if (h2)
        CloseHandle(h2);

    /* manual-reset flag: a satisfied wait must not reset the event */
    SetEvent(h);
    ret = WaitForSingleObject(h, 0);
    ok(ret == WAIT_OBJECT_0, "first wait %lu\n", ret);
    ret = WaitForSingleObject(h, 0);
    ok(ret == WAIT_OBJECT_0, "event reset by wait, got %lu\n", ret);
    CloseHandle(h);
}

static void test_CreateMutexExW(void)
{
    HANDLE h;
    DWORD ret;

    h = CreateMutexExW(NULL, NULL, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    ok(h != NULL, "CreateMutexExW failed %lu\n", GetLastError());
    if (!h)
        return;
    /* we own it: a recursive acquire succeeds immediately */
    ret = WaitForSingleObject(h, 0);
    ok(ret == WAIT_OBJECT_0, "owner wait returned %lu\n", ret);
    ReleaseMutex(h);
    ReleaseMutex(h);
    SetLastError(0xdeadbeef);
    ok(!ReleaseMutex(h), "released a mutex that is no longer owned\n");
    ok(GetLastError() == ERROR_NOT_OWNER, "got error %lu\n", GetLastError());
    CloseHandle(h);
}

static void test_ExplicitStubs(void)
{
    typedef BOOL (WINAPI *PFN_SetProcessGroupAffinity)(HANDLE, const GROUP_AFFINITY *, GROUP_AFFINITY *);
    typedef BOOL (WINAPI *PFN_SetThreadSelectedCpuSets)(HANDLE, const ULONG *, ULONG);
    PFN_SetProcessGroupAffinity pSetProcessGroupAffinity;
    PFN_SetThreadSelectedCpuSets pSetThreadSelectedCpuSets;
    GROUP_AFFINITY ga = { 1, 0 }, old;
    BOOL ret;

    /* Honest-contract check: an unimplemented API must not report success
     * without doing work. Wine's bodies for these are stubs too. */
    pSetProcessGroupAffinity = (PFN_SetProcessGroupAffinity)GetKernelBaseProc("SetProcessGroupAffinity");
    if (pSetProcessGroupAffinity)
    {
        SetLastError(0xdeadbeef);
        ret = pSetProcessGroupAffinity(GetCurrentProcess(), &ga, &old);
        ok(!ret || GetLastError() != 0xdeadbeef, "SetProcessGroupAffinity succeeded silently\n");
        if (!ret)
            ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED || GetLastError() == ERROR_INVALID_PARAMETER,
               "got error %lu\n", GetLastError());
    }
    else
        skip("SetProcessGroupAffinity not exported\n");

    pSetThreadSelectedCpuSets = (PFN_SetThreadSelectedCpuSets)GetKernelBaseProc("SetThreadSelectedCpuSets");
    if (pSetThreadSelectedCpuSets)
    {
        SetLastError(0xdeadbeef);
        ret = pSetThreadSelectedCpuSets(GetCurrentThread(), NULL, 0);
        /* count 0 clears the selection; accepting it is valid, a non-zero
         * count with CPU set ids we cannot honor must not be accepted blindly */
        trace("SetThreadSelectedCpuSets(NULL,0) -> %d (error %lu)\n", ret, GetLastError());
    }
    else
        skip("SetThreadSelectedCpuSets not exported\n");
}

static void test_VirtualMemoryHints(void)
{
    typedef DWORD (WINAPI *PFN_DiscardVirtualMemory)(PVOID, SIZE_T);
    PFN_DiscardVirtualMemory pDiscardVirtualMemory;
    PVOID mem;
    DWORD ret;

    pDiscardVirtualMemory = (PFN_DiscardVirtualMemory)GetKernelBaseProc("DiscardVirtualMemory");
    if (!pDiscardVirtualMemory)
    {
        skip("DiscardVirtualMemory not exported\n");
        return;
    }
    mem = VirtualAlloc(NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(mem != NULL, "VirtualAlloc failed %lu\n", GetLastError());
    if (!mem)
        return;
    ret = pDiscardVirtualMemory(mem, 0x2000);
    ok(ret == ERROR_SUCCESS, "DiscardVirtualMemory returned %lu\n", ret);
    ret = pDiscardVirtualMemory(NULL, 0x1000);
    ok(ret != ERROR_SUCCESS, "DiscardVirtualMemory(NULL) succeeded\n");
    VirtualFree(mem, 0, MEM_RELEASE);
}

START_TEST(WineGroups12)
{
    test_GetLogicalDriveStrings();
    test_DriveTypeInvalid();
    test_ConditionVariableTimeout();
    test_CreateEventExW();
    test_CreateMutexExW();
    test_ExplicitStubs();
    test_VirtualMemoryHints();
}
