/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tramo E: transacted file API, CreateFileMappingNuma, SetProcessGroupAffinity
 */

#include "precomp.h"

typedef HANDLE (WINAPI *PFN_CreateFileTransactedW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE, HANDLE, PUSHORT, PVOID);
typedef BOOL (WINAPI *PFN_TxFileW)(LPCWSTR, HANDLE);
typedef BOOL (WINAPI *PFN_CopyFileTransactedW)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_MoveFileTransactedW)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_CreateDirectoryTransactedW)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES, HANDLE);
typedef HANDLE (WINAPI *PFN_CreateFileMappingNumaW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR, DWORD);
typedef BOOL (WINAPI *PFN_SetProcessGroupAffinity)(HANDLE, const GROUP_AFFINITY *, PGROUP_AFFINITY);

static void test_Transacted(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_CreateFileTransactedW pCreateFile = (PFN_CreateFileTransactedW)GetProcAddress(k32, "CreateFileTransactedW");
    PFN_TxFileW pDelete = (PFN_TxFileW)GetProcAddress(k32, "DeleteFileTransactedW");
    PFN_TxFileW pRemoveDir = (PFN_TxFileW)GetProcAddress(k32, "RemoveDirectoryTransactedW");
    PFN_CopyFileTransactedW pCopy = (PFN_CopyFileTransactedW)GetProcAddress(k32, "CopyFileTransactedW");
    PFN_MoveFileTransactedW pMove = (PFN_MoveFileTransactedW)GetProcAddress(k32, "MoveFileTransactedW");
    PFN_CreateDirectoryTransactedW pMkDir = (PFN_CreateDirectoryTransactedW)GetProcAddress(k32, "CreateDirectoryTransactedW");
    WCHAR dir[MAX_PATH], a[MAX_PATH], b[MAX_PATH], sub[MAX_PATH];
    HANDLE h, fakeTx = (HANDLE)(ULONG_PTR)0x1234;
    DWORD written;

    if (!pCreateFile || !pDelete || !pRemoveDir || !pCopy || !pMove || !pMkDir)
    {
        skip("transacted APIs not exported\n");
        return;
    }

    GetTempPathW(ARRAYSIZE(dir), dir);
    wcscat(dir, L"TramoE");
    wcscpy(a, dir); wcscat(a, L"\\a.txt");
    wcscpy(b, dir); wcscat(b, L"\\b.txt");
    wcscpy(sub, dir); wcscat(sub, L"\\sub");
    CreateDirectoryW(dir, NULL);

    /* no transaction: the plain operation */
    h = pCreateFile(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL, NULL, NULL, NULL);
    ok(h != INVALID_HANDLE_VALUE, "CreateFileTransactedW(NULL tx) failed %lu\n", GetLastError());
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, "x", 1, &written, NULL);
        CloseHandle(h);
    }
    ok(pCopy(a, b, NULL, NULL, NULL, 0, NULL), "CopyFileTransactedW failed %lu\n", GetLastError());
    ok(GetFileAttributesW(b) != INVALID_FILE_ATTRIBUTES, "copy missing\n");
    ok(pDelete(b, NULL), "DeleteFileTransactedW failed %lu\n", GetLastError());
    ok(GetFileAttributesW(b) == INVALID_FILE_ATTRIBUTES, "file still present\n");
    ok(pMove(a, b, NULL, NULL, 0, NULL), "MoveFileTransactedW failed %lu\n", GetLastError());
    ok(pMkDir(NULL, sub, NULL, NULL), "CreateDirectoryTransactedW failed %lu\n", GetLastError());
    ok(pRemoveDir(sub, NULL), "RemoveDirectoryTransactedW failed %lu\n", GetLastError());

    /* a transaction handle can never be valid: fail honestly, change nothing */
    SetLastError(0xdeadbeef);
    h = pCreateFile(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL, fakeTx, NULL, NULL);
    ok(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_TRANSACTION, "tx create: %p error %lu\n", h, GetLastError());
    ok(GetFileAttributesW(a) == INVALID_FILE_ATTRIBUTES, "file created under a fake transaction\n");
    SetLastError(0xdeadbeef);
    ok(!pDelete(b, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx delete error %lu\n", GetLastError());
    ok(GetFileAttributesW(b) != INVALID_FILE_ATTRIBUTES, "file deleted under a fake transaction\n");
    SetLastError(0xdeadbeef);
    ok(!pMove(b, a, NULL, NULL, 0, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx move error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!pCopy(b, a, NULL, NULL, NULL, 0, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx copy error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!pMkDir(NULL, sub, NULL, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx mkdir error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!pRemoveDir(dir, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx rmdir error %lu\n", GetLastError());

    DeleteFileW(b);
    RemoveDirectoryW(dir);
}

static void test_FileMappingNuma(void)
{
    PFN_CreateFileMappingNumaW pNuma = (PFN_CreateFileMappingNumaW)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileMappingNumaW");
    HANDLE h;
    void *p;

    if (!pNuma)
    {
        skip("CreateFileMappingNumaW not exported\n");
        return;
    }
    h = pNuma(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, NULL, (DWORD)-1);
    ok(h != NULL, "no preferred node failed %lu\n", GetLastError());
    if (h)
    {
        p = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, 0);
        ok(p != NULL, "MapViewOfFile failed %lu\n", GetLastError());
        if (p)
            UnmapViewOfFile(p);
        CloseHandle(h);
    }
    h = pNuma(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, NULL, 0);
    ok(h != NULL, "node 0 failed %lu\n", GetLastError());
    if (h)
        CloseHandle(h);
    SetLastError(0xdeadbeef);
    h = pNuma(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, NULL, 0x7fff);
    ok(h == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "bad node: %p error %lu\n", h, GetLastError());
}

static void test_SetProcessGroupAffinity(void)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    PFN_SetProcessGroupAffinity pSet = kb ? (PFN_SetProcessGroupAffinity)GetProcAddress(kb, "SetProcessGroupAffinity") : NULL;
    GROUP_AFFINITY ga, old;
    DWORD_PTR procMask, sysMask, nowMask;

    if (!pSet)
    {
        skip("SetProcessGroupAffinity not exported\n");
        return;
    }
    ok(GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask), "GetProcessAffinityMask failed\n");

    SetLastError(0xdeadbeef);
    ok(!pSet(GetCurrentProcess(), NULL, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "NULL affinity\n");
    memset(&ga, 0, sizeof(ga));
    ga.Mask = procMask;
    ga.Group = 1;
    SetLastError(0xdeadbeef);
    ok(!pSet(GetCurrentProcess(), &ga, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "group 1 accepted\n");
    ga.Group = 0;
    ga.Mask = 0;
    SetLastError(0xdeadbeef);
    ok(!pSet(GetCurrentProcess(), &ga, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "empty mask accepted\n");

    ga.Mask = procMask;
    memset(&old, 0xcc, sizeof(old));
    ok(pSet(GetCurrentProcess(), &ga, &old), "SetProcessGroupAffinity failed %lu\n", GetLastError());
    ok(old.Group == 0 && old.Mask == procMask, "previous group %u mask %Ix, expected 0 / %Ix\n", old.Group, old.Mask, procMask);
    ok(GetProcessAffinityMask(GetCurrentProcess(), &nowMask, &sysMask), "GetProcessAffinityMask failed\n");
    ok(nowMask == ga.Mask, "mask not applied: %Ix\n", nowMask);
}

START_TEST(TramoE)
{
    test_Transacted();
    test_FileMappingNuma();
    test_SetProcessGroupAffinity();
}
