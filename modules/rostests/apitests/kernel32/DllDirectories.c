/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     AddDllDirectory and SetDefaultDllDirectories reach loads that give the loader no search path
 *
 * SetDefaultDllDirectories cannot be undone, so this test runs last and leaves the
 * process with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS.
 */

#include "precomp.h"

#define SEARCH_APPLICATION_DIR 0x00000200
#define SEARCH_USER_DIRS       0x00000400
#define SEARCH_SYSTEM32        0x00000800
#define SEARCH_DEFAULT_DIRS    0x00001000

typedef PVOID (WINAPI *PFN_ADD)(PCWSTR);
typedef BOOL (WINAPI *PFN_REMOVE)(PVOID);
typedef BOOL (WINAPI *PFN_SETDEFAULT)(DWORD);
typedef NTSTATUS (NTAPI *PFN_LDRLOAD)(PWSTR, PULONG, PUNICODE_STRING, PVOID *);
typedef NTSTATUS (NTAPI *PFN_LDRADD)(PUNICODE_STRING, PVOID *);
typedef NTSTATUS (NTAPI *PFN_LDRREMOVE)(PVOID);
typedef NTSTATUS (NTAPI *PFN_LDRSETDEFAULT)(ULONG);

static PFN_ADD pAdd;
static PFN_REMOVE pRemove;
static PFN_SETDEFAULT pSetDefault;
static PFN_LDRLOAD pLdrLoad;
static PFN_LDRADD pLdrAdd;
static PFN_LDRREMOVE pLdrRemove;
static PFN_LDRSETDEFAULT pLdrSetDefault;

static WCHAR g_TempDir[MAX_PATH];
static WCHAR g_DllCopy[MAX_PATH];
static const WCHAR g_DllName[] = L"ldrdirtest.dll";

static
BOOL
MakeTestDll(void)
{
    WCHAR Temp[MAX_PATH], Source[MAX_PATH];

    if (!GetTempPathW(ARRAYSIZE(Temp), Temp))
        return FALSE;
    StringCchPrintfW(g_TempDir, ARRAYSIZE(g_TempDir), L"%lsldrdirtest.%lu", Temp, GetCurrentProcessId());
    if (!CreateDirectoryW(g_TempDir, NULL))
        return FALSE;

    GetSystemDirectoryW(Source, ARRAYSIZE(Source));
    StringCchCatW(Source, ARRAYSIZE(Source), L"\\version.dll");
    StringCchPrintfW(g_DllCopy, ARRAYSIZE(g_DllCopy), L"%ls\\%ls", g_TempDir, g_DllName);
    if (!CopyFileW(Source, g_DllCopy, FALSE))
    {
        RemoveDirectoryW(g_TempDir);
        return FALSE;
    }
    return TRUE;
}

static
void
RemoveTestDll(void)
{
    DeleteFileW(g_DllCopy);
    RemoveDirectoryW(g_TempDir);
}

/* A load with no search path at all, like a loader-internal or direct Ldr caller */
static
NTSTATUS
LoadWithoutPath(PVOID *Base)
{
    UNICODE_STRING Name;

    RtlInitUnicodeString(&Name, g_DllName);
    *Base = NULL;
    return pLdrLoad(NULL, NULL, &Name, Base);
}

static
void
Test_Invalid(void)
{
    UNICODE_STRING Dir;
    PVOID Cookie = (PVOID)(ULONG_PTR)0x1234;
    NTSTATUS Status;
    BOOL Result;

    /* The documented argument rules of the Win32 calls */
    SetLastError(0xDEADBEEF);
    ok(pAdd(L"relative\\dir") == NULL, "A relative directory was accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Result = pSetDefault(0);
    ok(!Result, "SetDefaultDllDirectories(0) succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Result = pSetDefault(0x1);
    ok(!Result, "A flag that is not a default directory flag was accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    /* The loader entry points refuse the same input */
    Status = pLdrSetDefault(0);
    ok(!NT_SUCCESS(Status), "LdrSetDefaultDllDirectories(0) returned %lx\n", Status);

    RtlInitUnicodeString(&Dir, L"relative\\dir");
    Status = pLdrAdd(&Dir, &Cookie);
    ok(!NT_SUCCESS(Status), "LdrAddDllDirectory accepted a relative directory: %lx\n", Status);

    Status = pLdrAdd(NULL, &Cookie);
    ok(!NT_SUCCESS(Status), "LdrAddDllDirectory accepted a NULL directory: %lx\n", Status);

    RtlInitUnicodeString(&Dir, L"C:\\ldrdirtest-this-directory-does-not-exist");
    Status = pLdrAdd(&Dir, &Cookie);
    ok(!NT_SUCCESS(Status), "LdrAddDllDirectory accepted a directory that does not exist: %lx\n", Status);

    Status = pLdrRemove((PVOID)(ULONG_PTR)0x1234);
    ok(!NT_SUCCESS(Status), "LdrRemoveDllDirectory accepted a cookie it never gave: %lx\n", Status);
}

static
void
Test_Directories(void)
{
    PVOID Cookie, Base;
    NTSTATUS Status;

    /* Nothing chose a default yet: the file is in no directory the loader looks in */
    Status = LoadWithoutPath(&Base);
    ok(!NT_SUCCESS(Status), "%ls was found before its directory was added: %lx\n", g_DllName, Status);
    if (NT_SUCCESS(Status))
        FreeLibrary(Base);

    Cookie = pAdd(g_TempDir);
    ok(Cookie != NULL, "AddDllDirectory failed: %lu\n", GetLastError());
    if (!Cookie)
        return;

    /* A user directory alone is not searched until the process says so */
    ok(pSetDefault(SEARCH_USER_DIRS), "SetDefaultDllDirectories failed: %lu\n", GetLastError());
    Status = LoadWithoutPath(&Base);
    ok(NT_SUCCESS(Status), "%ls was not found in the user directory: %lx\n", g_DllName, Status);
    if (NT_SUCCESS(Status))
        FreeLibrary(Base);

    /* Take the directory away again */
    ok(pRemove(Cookie), "RemoveDllDirectory failed: %lu\n", GetLastError());
    Status = LoadWithoutPath(&Base);
    ok(!NT_SUCCESS(Status), "%ls was found after its directory was removed: %lx\n", g_DllName, Status);
    if (NT_SUCCESS(Status))
        FreeLibrary(Base);

    /* A directory is only used when user directories are part of the default */
    Cookie = pAdd(g_TempDir);
    ok(Cookie != NULL, "AddDllDirectory failed: %lu\n", GetLastError());
    ok(pSetDefault(SEARCH_SYSTEM32), "SetDefaultDllDirectories failed: %lu\n", GetLastError());
    Status = LoadWithoutPath(&Base);
    ok(!NT_SUCCESS(Status), "%ls was found although only system32 is searched: %lx\n", g_DllName, Status);
    if (NT_SUCCESS(Status))
        FreeLibrary(Base);

    ok(pSetDefault(SEARCH_DEFAULT_DIRS), "SetDefaultDllDirectories failed: %lu\n", GetLastError());
    Status = LoadWithoutPath(&Base);
    ok(NT_SUCCESS(Status), "%ls was not found with the default directories: %lx\n", g_DllName, Status);
    if (NT_SUCCESS(Status))
        FreeLibrary(Base);

    if (Cookie)
        pRemove(Cookie);
}

START_TEST(DllDirectories)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    pAdd = (PFN_ADD)GetProcAddress(Kernel32, "AddDllDirectory");
    pRemove = (PFN_REMOVE)GetProcAddress(Kernel32, "RemoveDllDirectory");
    pSetDefault = (PFN_SETDEFAULT)GetProcAddress(Kernel32, "SetDefaultDllDirectories");
    pLdrLoad = (PFN_LDRLOAD)GetProcAddress(Ntdll, "LdrLoadDll");
    pLdrAdd = (PFN_LDRADD)GetProcAddress(Ntdll, "LdrAddDllDirectory");
    pLdrRemove = (PFN_LDRREMOVE)GetProcAddress(Ntdll, "LdrRemoveDllDirectory");
    pLdrSetDefault = (PFN_LDRSETDEFAULT)GetProcAddress(Ntdll, "LdrSetDefaultDllDirectories");

    ok(pAdd && pRemove && pSetDefault, "A kernel32 DLL directory function is missing\n");
    ok(pLdrAdd && pLdrRemove && pLdrSetDefault, "A loader DLL directory function is not exported\n");
    if (!pAdd || !pRemove || !pSetDefault || !pLdrLoad || !pLdrAdd || !pLdrRemove || !pLdrSetDefault)
        return;

    Test_Invalid();

    if (!MakeTestDll())
    {
        skip("Could not prepare the test DLL\n");
        return;
    }

    Test_Directories();
    RemoveTestDll();
}
