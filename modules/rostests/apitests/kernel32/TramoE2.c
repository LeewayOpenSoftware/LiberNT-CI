/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tramo E2: recovery registration, hard link names, remaining transacted
 *              entry points, GetNamedPipeClientComputerNameA, QueryIoRingCapabilities
 */

#include "precomp.h"

typedef HRESULT (WINAPI *PFN_Register)(APPLICATION_RECOVERY_CALLBACK, PVOID, DWORD, DWORD);
typedef HRESULT (WINAPI *PFN_Unregister)(VOID);
typedef HRESULT (WINAPI *PFN_GetRecovery)(HANDLE, APPLICATION_RECOVERY_CALLBACK *, PVOID *, DWORD *, DWORD *);
typedef HANDLE (WINAPI *PFN_FindFirstFileNameW)(LPCWSTR, DWORD, LPDWORD, PWSTR);
typedef BOOL (WINAPI *PFN_FindNextFileNameW)(HANDLE, LPDWORD, PWSTR);
typedef DWORD (WINAPI *PFN_GetFullPathNameTransactedW)(LPCWSTR, DWORD, LPWSTR, LPWSTR *, HANDLE);
typedef BOOL (WINAPI *PFN_SetAttrTransactedW)(LPCWSTR, DWORD, HANDLE);
typedef BOOL (WINAPI *PFN_GetAttrTransactedW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID, HANDLE);
typedef BOOL (WINAPI *PFN_PipeClientA)(HANDLE, LPSTR, ULONG);
typedef HRESULT (WINAPI *PFN_QueryIoRing)(PVOID);

static DWORD WINAPI RecoveryCallback(PVOID param)
{
    return 0;
}

static void test_Recovery(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_Register pRegister = (PFN_Register)GetProcAddress(k32, "RegisterApplicationRecoveryCallback");
    PFN_Unregister pUnregister = (PFN_Unregister)GetProcAddress(k32, "UnregisterApplicationRecoveryCallback");
    PFN_GetRecovery pGet = (PFN_GetRecovery)GetProcAddress(k32, "GetApplicationRecoveryCallback");
    APPLICATION_RECOVERY_CALLBACK cb = NULL;
    PVOID param = NULL;
    DWORD ping = 0, flags = 0xff;
    HRESULT hr;

    if (!pRegister || !pUnregister || !pGet)
    {
        skip("application recovery APIs not exported\n");
        return;
    }

    ok(pRegister(NULL, NULL, 0, 0) == E_INVALIDARG, "NULL callback accepted\n");
    ok(pRegister(RecoveryCallback, NULL, 0, 1) == E_INVALIDARG, "flags accepted\n");
    ok(pRegister(RecoveryCallback, NULL, 6 * 60 * 1000, 0) == E_INVALIDARG, "ping over five minutes accepted\n");

    hr = pRegister(RecoveryCallback, (PVOID)(ULONG_PTR)0x77, 0, 0);
    ok(hr == S_OK, "Register returned %lx\n", hr);
    hr = pGet(GetCurrentProcess(), &cb, &param, &ping, &flags);
    ok(hr == S_OK, "Get returned %lx\n", hr);
    ok(cb == RecoveryCallback && param == (PVOID)(ULONG_PTR)0x77, "wrong registration %p %p\n", cb, param);
    ok(ping == 5000 && flags == 0, "ping %lu flags %lu, expected default 5000 / 0\n", ping, flags);

    ok(pUnregister() == S_OK, "Unregister failed\n");
    cb = NULL;
    ok(pGet(GetCurrentProcess(), &cb, &param, &ping, &flags) == E_FAIL && cb == NULL, "registration survived Unregister\n");
}

static void test_HardLinkNames(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_FindFirstFileNameW pFirst = (PFN_FindFirstFileNameW)GetProcAddress(k32, "FindFirstFileNameW");
    PFN_FindNextFileNameW pNext = (PFN_FindNextFileNameW)GetProcAddress(k32, "FindNextFileNameW");
    WCHAR dir[MAX_PATH], a[MAX_PATH], b[MAX_PATH], name[MAX_PATH];
    DWORD len;
    HANDLE h, find;
    DWORD written;
    int count;

    if (!pFirst || !pNext)
    {
        skip("FindFirstFileNameW/FindNextFileNameW not exported\n");
        return;
    }

    SetLastError(0xdeadbeef);
    len = ARRAYSIZE(name);
    ok(pFirst(NULL, 0, &len, name) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER, "NULL name\n");
    SetLastError(0xdeadbeef);
    ok(pFirst(L"C:\\x", 1, &len, name) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER, "flags accepted\n");
    SetLastError(0xdeadbeef);
    ok(!pNext(INVALID_HANDLE_VALUE, &len, name) && GetLastError() == ERROR_INVALID_HANDLE, "bad handle\n");

    GetTempPathW(ARRAYSIZE(dir), dir);
    wcscat(dir, L"TramoE2");
    CreateDirectoryW(dir, NULL);
    wcscpy(a, dir); wcscat(a, L"\\a.txt");
    wcscpy(b, dir); wcscat(b, L"\\b.txt");
    h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, "x", 1, &written, NULL);
        CloseHandle(h);
    }
    if (!CreateHardLinkW(b, a, NULL))
    {
        skip("file system has no hard links (error %lu)\n", GetLastError());
        goto done;
    }

    len = ARRAYSIZE(name);
    find = pFirst(a, 0, &len, name);
    if (find == INVALID_HANDLE_VALUE)
    {
        skip("file system does not report hard link names (error %lu)\n", GetLastError());
        goto done;
    }
    ok(name[0] == L'\\', "link name is not volume relative: %S\n", name);
    count = 1;

    /* too short a buffer: the needed size is reported, the position is kept */
    len = 1;
    SetLastError(0xdeadbeef);
    ok(!pNext(find, &len, name) && GetLastError() == ERROR_MORE_DATA && len > 1, "short buffer: error %lu len %lu\n", GetLastError(), len);
    len = ARRAYSIZE(name);
    while (pNext(find, &len, name))
    {
        count++;
        len = ARRAYSIZE(name);
    }
    ok(GetLastError() == ERROR_HANDLE_EOF, "end of list gave error %lu\n", GetLastError());
    ok(count == 2, "expected 2 names for a file with 2 links, got %d\n", count);
    ok(FindClose(find), "FindClose failed %lu\n", GetLastError());

done:
    DeleteFileW(b);
    DeleteFileW(a);
    RemoveDirectoryW(dir);
}

static void test_MoreTransacted(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GetFullPathNameTransactedW pFull = (PFN_GetFullPathNameTransactedW)GetProcAddress(k32, "GetFullPathNameTransactedW");
    PFN_SetAttrTransactedW pSetAttr = (PFN_SetAttrTransactedW)GetProcAddress(k32, "SetFileAttributesTransactedW");
    PFN_GetAttrTransactedW pGetAttr = (PFN_GetAttrTransactedW)GetProcAddress(k32, "GetFileAttributesTransactedW");
    WCHAR full[MAX_PATH], expect[MAX_PATH], *part;
    WIN32_FILE_ATTRIBUTE_DATA data;
    HANDLE fakeTx = (HANDLE)(ULONG_PTR)0x1234;
    DWORD ret;

    if (!pFull || !pSetAttr || !pGetAttr)
    {
        skip("transacted path/attribute APIs not exported\n");
        return;
    }

    ret = pFull(L"C:\\Windows\\..\\x.txt", ARRAYSIZE(full), full, &part, NULL);
    ok(ret != 0, "GetFullPathNameTransactedW failed %lu\n", GetLastError());
    GetFullPathNameW(L"C:\\Windows\\..\\x.txt", ARRAYSIZE(expect), expect, &part);
    ok(!wcscmp(full, expect), "got %S expected %S\n", full, expect);
    ok(pGetAttr(L"C:\\", GetFileExInfoStandard, &data, NULL), "GetFileAttributesTransactedW failed %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(pFull(L"C:\\x.txt", ARRAYSIZE(full), full, &part, fakeTx) == 0 && GetLastError() == ERROR_INVALID_TRANSACTION, "tx fullpath error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!pSetAttr(L"C:\\x.txt", FILE_ATTRIBUTE_NORMAL, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx setattr error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!pGetAttr(L"C:\\", GetFileExInfoStandard, &data, fakeTx) && GetLastError() == ERROR_INVALID_TRANSACTION, "tx getattr error %lu\n", GetLastError());
}

static void test_PipeAndIoRing(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_PipeClientA pPipeA = (PFN_PipeClientA)GetProcAddress(k32, "GetNamedPipeClientComputerNameA");
    PFN_QueryIoRing pIoRing = (PFN_QueryIoRing)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryIoRingCapabilities");
    char name[32];
    BYTE caps[16];
    HRESULT hr;

    if (pPipeA)
    {
        SetLastError(0xdeadbeef);
        ok(!pPipeA(INVALID_HANDLE_VALUE, name, sizeof(name)), "GetNamedPipeClientComputerNameA succeeded on an invalid handle\n");
        ok(GetLastError() != 0xdeadbeef, "no error set\n");
    }
    else
        skip("GetNamedPipeClientComputerNameA not exported\n");

    if (pIoRing)
    {
        memset(caps, 0xcc, sizeof(caps));
        hr = pIoRing(caps);
        ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "QueryIoRingCapabilities returned %lx\n", hr);
        ok(caps[0] == 0 && caps[15] == 0, "capabilities not cleared\n");
        ok(pIoRing(NULL) == E_POINTER, "NULL not rejected\n");
    }
    else
        skip("QueryIoRingCapabilities not exported\n");
}

START_TEST(TramoE2)
{
    test_Recovery();
    test_HardLinkNames();
    test_MoreTransacted();
    test_PipeAndIoRing();
}
