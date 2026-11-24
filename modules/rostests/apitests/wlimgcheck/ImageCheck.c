/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the wlimgcheck structural image verification
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <wlimgcheck.h>

static WCHAR g_TempDir[MAX_PATH];

static BOOL
MakeTempPath(
    _Out_writes_(MAX_PATH) PWSTR Path,
    _In_ PCWSTR Name)
{
    return SUCCEEDED(StringCchPrintfW(Path, MAX_PATH, L"%s\\%s", g_TempDir, Name));
}

static BOOL
WriteWholeFile(
    _In_ PCWSTR Path,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    HANDLE h;
    DWORD Written;
    BOOL Ok;

    h = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(h, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(h);
    return Ok;
}

static PBYTE
ReadWholeFile(
    _In_ PCWSTR Path,
    _Out_ PDWORD Size)
{
    HANDLE h;
    LARGE_INTEGER li;
    PBYTE Data = NULL;
    DWORD Read;

    *Size = 0;
    h = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(h, &li) && li.QuadPart > 0 && li.QuadPart < 64 * 1024 * 1024)
    {
        Data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)li.QuadPart);
        if (Data && (!ReadFile(h, Data, (DWORD)li.QuadPart, &Read, NULL) || Read != (DWORD)li.QuadPart))
        {
            HeapFree(GetProcessHeap(), 0, Data);
            Data = NULL;
        }
        else if (Data)
        {
            *Size = (DWORD)li.QuadPart;
        }
    }
    CloseHandle(h);
    return Data;
}

/* Restrict a file or directory to SYSTEM + Administrators (full), nothing else, no inheritance. */
static BOOL
SetRestrictedDacl(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Dacl = NULL;
    BOOL Present, Defaulted;
    DWORD Err;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &Sd, NULL))
        return FALSE;
    if (!GetSecurityDescriptorDacl(Sd, &Present, &Dacl, &Defaulted) || !Present)
    {
        LocalFree(Sd);
        return FALSE;
    }
    Err = SetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, Dacl, NULL);
    LocalFree(Sd);
    return Err == ERROR_SUCCESS;
}

/* Add an allow-write entry for Users on top of the current DACL. */
static BOOL
GrantUsersWrite(
    _In_ PCWSTR Path)
{
    EXPLICIT_ACCESSW Ea;
    PACL OldDacl = NULL, NewDacl = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    DWORD Err;

    if (GetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &OldDacl, NULL, &Sd) != ERROR_SUCCESS)
        return FALSE;

    ZeroMemory(&Ea, sizeof(Ea));
    Ea.grfAccessPermissions = FILE_GENERIC_WRITE;
    Ea.grfAccessMode = GRANT_ACCESS;
    Ea.grfInheritance = NO_INHERITANCE;
    Ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    Ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    Ea.Trustee.ptstrName = (PWSTR)L"Users";

    Err = SetEntriesInAclW(1, &Ea, OldDacl, &NewDacl);
    if (Err == ERROR_SUCCESS)
        Err = SetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    NULL, NULL, NewDacl, NULL);
    if (NewDacl) LocalFree(NewDacl);
    LocalFree(Sd);
    return Err == ERROR_SUCCESS;
}

static void
Test_PathRules(void)
{
    HANDLE h = (HANDLE)0x1234;
    WLIMG_STATUS Status;
    WCHAR SysDir[MAX_PATH];
    WCHAR Path[MAX_PATH];

    Status = WlImgOpenVerified(L"notepad.exe", 0, &h);
    ok(Status == WlImgBadPath, "relative: %u\n", Status);
    ok(h == INVALID_HANDLE_VALUE, "relative: handle %p\n", h);

    Status = WlImgOpenVerified(L"C:\\Windows\\..\\Windows\\System32\\notepad.exe", 0, &h);
    ok(Status == WlImgBadPath, "dotdot: %u\n", Status);

    GetSystemDirectoryW(SysDir, ARRAYSIZE(SysDir));
    /* A subdirectory of System32 is not "directly in System32". */
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\drivers\\etc\\hosts", SysDir);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32, &h);
    ok(Status == WlImgBadPath, "subdir of System32: %u\n", Status);

    MakeTempPath(Path, L"outside.exe");
    WriteWholeFile(Path, "MZ", 2);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32, &h);
    ok(Status == WlImgBadPath, "outside System32 with REQUIRE: %u\n", Status);
    DeleteFileW(Path);

    MakeTempPath(Path, L"missing.exe");
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOpenFailed, "missing: %u\n", Status);
}

static void
Test_PeRules(void)
{
    WCHAR SysDir[MAX_PATH], Src[MAX_PATH], Path[MAX_PATH];
    HANDLE h;
    WLIMG_STATUS Status;
    PBYTE Data;
    DWORD Size;
    PIMAGE_DOS_HEADER Dos;
    PIMAGE_NT_HEADERS Nt;
    WORD OriginalMachine;

    GetSystemDirectoryW(SysDir, ARRAYSIZE(SysDir));

    /* Not a PE at all. */
    MakeTempPath(Path, L"text.exe");
    WriteWholeFile(Path, "hello world, not a program", 26);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotPe, "text: %u\n", Status);
    DeleteFileW(Path);

    /* Truncated: MZ header only. */
    MakeTempPath(Path, L"short.exe");
    {
        IMAGE_DOS_HEADER d; ZeroMemory(&d, sizeof(d)); d.e_magic = IMAGE_DOS_SIGNATURE; d.e_lfanew = 0x80;
        WriteWholeFile(Path, &d, sizeof(d));
    }
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotPe, "truncated: %u\n", Status);
    DeleteFileW(Path);

    /* Positive fixture: a native GUI exe from System32, copied so its DACL is ours. */
    StringCchPrintfW(Src, ARRAYSIZE(Src), L"%s\\notepad.exe", SysDir);
    Data = ReadWholeFile(Src, &Size);
    if (Data == NULL)
    {
        skip("no %ls to use as a GUI fixture\n", Src);
        return;
    }

    MakeTempPath(Path, L"gui.exe");
    ok(WriteWholeFile(Path, Data, Size), "write gui fixture\n");
    if (!SetRestrictedDacl(Path))
    {
        skip("cannot set a restricted DACL (not elevated?)\n");
        DeleteFileW(Path);
        HeapFree(GetProcessHeap(), 0, Data);
        return;
    }
    h = NULL;
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOk, "gui fixture: %u (%lu)\n", Status, GetLastError());
    ok(h != NULL && h != INVALID_HANDLE_VALUE, "gui fixture: handle %p\n", h);
    if (Status == WlImgOk)
    {
        WCHAR Renamed[MAX_PATH];
        MakeTempPath(Renamed, L"gui-renamed.exe");

        /* P5: while the verified handle is open, a rename must fail. */
        ok(!MoveFileExW(Path, Renamed, 0), "rename succeeded while handle open\n");
        ok(GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED,
           "rename error %lu\n", GetLastError());
        CloseHandle(h);
        ok(MoveFileExW(Path, Renamed, 0), "rename after close failed %lu\n", GetLastError());
        MoveFileExW(Renamed, Path, 0);
    }

    /* Same bytes, DACL that lets Users write -> rejected. */
    ok(GrantUsersWrite(Path), "grant users write\n");
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgFileWritable, "users-writable: %u\n", Status);
    ok(h == INVALID_HANDLE_VALUE, "users-writable: handle %p\n", h);
    SetRestrictedDacl(Path);

    /* Wrong machine: patch FileHeader.Machine. */
    Dos = (PIMAGE_DOS_HEADER)Data;
    Nt = (PIMAGE_NT_HEADERS)(Data + Dos->e_lfanew);
    OriginalMachine = Nt->FileHeader.Machine;
    Nt->FileHeader.Machine = 0x1234;
    MakeTempPath(Path, L"machine.exe");
    WriteWholeFile(Path, Data, Size);
    SetRestrictedDacl(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgWrongMachine, "wrong machine: %u\n", Status);
    DeleteFileW(Path);

    /* Right machine, but a DLL. */
    Nt->FileHeader.Machine = OriginalMachine;
    Nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
    MakeTempPath(Path, L"dll.exe");
    WriteWholeFile(Path, Data, Size);
    SetRestrictedDacl(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotGuiExe, "dll: %u\n", Status);
    DeleteFileW(Path);
    Nt->FileHeader.Characteristics &= ~IMAGE_FILE_DLL;

    /* Console subsystem is not a logon UI binary either. */
    Nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    MakeTempPath(Path, L"cui.exe");
    WriteWholeFile(Path, Data, Size);
    SetRestrictedDacl(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotGuiExe, "cui: %u\n", Status);
    DeleteFileW(Path);

    MakeTempPath(Path, L"gui.exe");
    DeleteFileW(Path);
    HeapFree(GetProcessHeap(), 0, Data);
}

static void
Test_DirectoryRule(void)
{
    WCHAR SysDir[MAX_PATH], Src[MAX_PATH], Dir[MAX_PATH], Path[MAX_PATH];
    HANDLE h;
    WLIMG_STATUS Status;
    PBYTE Data;
    DWORD Size;

    GetSystemDirectoryW(SysDir, ARRAYSIZE(SysDir));
    StringCchPrintfW(Src, ARRAYSIZE(Src), L"%s\\notepad.exe", SysDir);
    Data = ReadWholeFile(Src, &Size);
    if (Data == NULL)
    {
        skip("no GUI fixture\n");
        return;
    }

    MakeTempPath(Dir, L"wlimg-dir");
    CreateDirectoryW(Dir, NULL);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\gui.exe", Dir);
    WriteWholeFile(Path, Data, Size);
    HeapFree(GetProcessHeap(), 0, Data);

    if (!SetRestrictedDacl(Path) || !SetRestrictedDacl(Dir))
    {
        skip("cannot set restricted DACLs (not elevated?)\n");
        DeleteFileW(Path);
        RemoveDirectoryW(Dir);
        return;
    }

    Status = WlImgOpenVerified(Path, WLIMG_CHECK_DIRECTORY, &h);
    ok(Status == WlImgOk, "restricted dir: %u\n", Status);
    if (Status == WlImgOk)
        CloseHandle(h);

    ok(GrantUsersWrite(Dir), "grant users write on dir\n");
    Status = WlImgOpenVerified(Path, WLIMG_CHECK_DIRECTORY, &h);
    ok(Status == WlImgDirectoryWritable, "users-writable dir: %u\n", Status);

    /* Without the directory flag the same file still passes on its own DACL. */
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOk, "file only: %u\n", Status);
    if (Status == WlImgOk)
        CloseHandle(h);

    SetRestrictedDacl(Dir);
    DeleteFileW(Path);
    RemoveDirectoryW(Dir);
}

START_TEST(ImageCheck)
{
    WCHAR Base[MAX_PATH];

    GetTempPathW(ARRAYSIZE(Base), Base);
    StringCchPrintfW(g_TempDir, ARRAYSIZE(g_TempDir), L"%swlimgcheck-%lu", Base, GetCurrentProcessId());
    CreateDirectoryW(g_TempDir, NULL);

    ok(wcscmp(WlImgStatusName(WlImgOk), L"ok") == 0, "status name\n");

    Test_PathRules();
    Test_PeRules();
    Test_DirectoryRule();

    RemoveDirectoryW(g_TempDir);
}
