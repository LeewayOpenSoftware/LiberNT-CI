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
#include <winioctl.h>
#include <wlimgcheck.h>

static WCHAR g_TempDir[MAX_PATH];
static PBYTE g_Gui;          /* a native GUI exe from System32, read once */
static DWORD g_GuiSize;

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

/* Owner Administrators, DACL SYSTEM + Administrators full, protected: the deployment shape. */
static BOOL
SetRestrictedSecurity(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PACL Dacl = NULL;
    PSID Owner = NULL;
    BOOL Present, Defaulted;
    DWORD Err;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &Sd, NULL))
        return FALSE;
    if (!GetSecurityDescriptorDacl(Sd, &Present, &Dacl, &Defaulted) || !Present ||
        !GetSecurityDescriptorOwner(Sd, &Owner, &Defaulted))
    {
        LocalFree(Sd);
        return FALSE;
    }
    Err = SetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                PROTECTED_DACL_SECURITY_INFORMATION,
                                Owner, NULL, Dacl, NULL);
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

static BOOL
EnablePrivilege(
    _In_ PCWSTR Name)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Tp;
    BOOL Ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return FALSE;
    Tp.PrivilegeCount = 1;
    Tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, Name, &Tp.Privileges[0].Luid))
        Ok = AdjustTokenPrivileges(Token, FALSE, &Tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(Token);
    return Ok;
}

/* A directory junction (mount point) needs no privilege; this is what an attacker can plant. */
typedef struct _TEST_MOUNT_POINT_REPARSE
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
} TEST_MOUNT_POINT_REPARSE;

static BOOL
MakeJunction(
    _In_ PCWSTR Link,
    _In_ PCWSTR Target)
{
    BYTE Buffer[sizeof(TEST_MOUNT_POINT_REPARSE) + 2 * MAX_PATH * sizeof(WCHAR) + 16];
    TEST_MOUNT_POINT_REPARSE *Rp = (TEST_MOUNT_POINT_REPARSE *)Buffer;
    WCHAR Subst[MAX_PATH + 8];
    USHORT SubstBytes, PrintBytes;
    HANDLE h;
    DWORD Returned;
    BOOL Ok;

    if (!CreateDirectoryW(Link, NULL))
        return FALSE;
    StringCchPrintfW(Subst, ARRAYSIZE(Subst), L"\\??\\%s", Target);
    SubstBytes = (USHORT)(wcslen(Subst) * sizeof(WCHAR));
    PrintBytes = (USHORT)(wcslen(Target) * sizeof(WCHAR));

    ZeroMemory(Buffer, sizeof(Buffer));
    Rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Rp->SubstituteNameOffset = 0;
    Rp->SubstituteNameLength = SubstBytes;
    Rp->PrintNameOffset = SubstBytes + sizeof(WCHAR);
    Rp->PrintNameLength = PrintBytes;
    CopyMemory(Rp->PathBuffer, Subst, SubstBytes);
    CopyMemory((PBYTE)Rp->PathBuffer + Rp->PrintNameOffset, Target, PrintBytes);
    Rp->ReparseDataLength = (USHORT)(8 + SubstBytes + sizeof(WCHAR) + PrintBytes + sizeof(WCHAR));

    h = CreateFileW(Link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(Link);
        return FALSE;
    }
    Ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, Rp, Rp->ReparseDataLength + 8, NULL, 0, &Returned, NULL);
    CloseHandle(h);
    if (!Ok)
        RemoveDirectoryW(Link);
    return Ok;
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
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\drivers\\etc\\hosts", SysDir);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32, &h);
    ok(Status == WlImgBadPath, "subdir of System32: %u\n", Status);

    /* R4c: an alternate data stream is refused before the open. */
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\notepad.exe:x", SysDir);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32, &h);
    ok(Status == WlImgBadPath, "ADS: %u\n", Status);

    MakeTempPath(Path, L"outside.exe");
    WriteWholeFile(Path, "MZ", 2);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32, &h);
    ok(Status == WlImgBadPath, "outside System32 with REQUIRE: %u\n", Status);
    DeleteFileW(Path);

    MakeTempPath(Path, L"missing.exe");
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOpenFailed, "missing: %u\n", Status);
}

/* R4a: the deployment shape. Must pass on Orosi as well as on Win11; a failure here
 * means System32 ownership or ACLs, not this library. */
static void
Test_System32Deployment(void)
{
    WCHAR SysDir[MAX_PATH], Path[MAX_PATH], Renamed[MAX_PATH];
    HANDLE h = NULL;
    WLIMG_STATUS Status;

    GetSystemDirectoryW(SysDir, ARRAYSIZE(SysDir));
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\notepad.exe", SysDir);
    Status = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32 | WLIMG_CHECK_DIRECTORY, &h);
    ok(Status == WlImgOk, "System32\\notepad.exe: %ls (%u, gle %lu)\n", WlImgStatusName(Status), Status, GetLastError());
    if (Status != WlImgOk)
        return;

    /* P5 on the real file: nothing may rename it while the verified handle is open. */
    StringCchPrintfW(Renamed, ARRAYSIZE(Renamed), L"%s\\notepad.exe.wlimg", SysDir);
    ok(!MoveFileExW(Path, Renamed, 0), "rename of a verified System32 file succeeded\n");
    if (GetFileAttributesW(Renamed) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(Renamed, Path, 0);
    CloseHandle(h);
}

static void
Test_PeRules(void)
{
    WCHAR Path[MAX_PATH];
    HANDLE h;
    WLIMG_STATUS Status;
    PBYTE Data;
    PIMAGE_DOS_HEADER Dos;
    PIMAGE_NT_HEADERS Nt;
    WORD OriginalMachine;

    MakeTempPath(Path, L"text.exe");
    WriteWholeFile(Path, "hello world, not a program", 26);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotPe, "text: %u\n", Status);
    DeleteFileW(Path);

    MakeTempPath(Path, L"short.exe");
    {
        IMAGE_DOS_HEADER d; ZeroMemory(&d, sizeof(d)); d.e_magic = IMAGE_DOS_SIGNATURE; d.e_lfanew = 0x80;
        WriteWholeFile(Path, &d, sizeof(d));
    }
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotPe, "truncated: %u\n", Status);
    DeleteFileW(Path);

    if (g_Gui == NULL)
    {
        skip("no GUI fixture\n");
        return;
    }
    Data = HeapAlloc(GetProcessHeap(), 0, g_GuiSize);
    if (Data == NULL)
        return;
    CopyMemory(Data, g_Gui, g_GuiSize);

    MakeTempPath(Path, L"gui.exe");
    ok(WriteWholeFile(Path, Data, g_GuiSize), "write gui fixture\n");
    if (!SetRestrictedSecurity(Path))
    {
        skip("cannot set owner/DACL on fixtures (not elevated?)\n");
        DeleteFileW(Path);
        HeapFree(GetProcessHeap(), 0, Data);
        return;
    }
    h = NULL;
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOk, "gui fixture: %u (%lu)\n", Status, GetLastError());
    if (Status == WlImgOk)
    {
        WCHAR Renamed[MAX_PATH];
        MakeTempPath(Renamed, L"gui-renamed.exe");
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
    SetRestrictedSecurity(Path);

    /* R4d: clean DACL but a non-admin owner -> rejected (owner holds WRITE_DAC). */
    if (EnablePrivilege(SE_RESTORE_NAME))
    {
        PSID Users = NULL;
        ConvertStringSidToSidW(L"S-1-5-32-545", &Users);
        if (SetNamedSecurityInfoW(Path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, Users, NULL, NULL, NULL) == ERROR_SUCCESS)
        {
            Status = WlImgOpenVerified(Path, 0, &h);
            ok(Status == WlImgFileWritable, "users-owned: %u\n", Status);
            SetRestrictedSecurity(Path);
        }
        else
            skip("cannot set a Users owner (%lu)\n", GetLastError());
        LocalFree(Users);
    }
    else
        skip("no SeRestorePrivilege for the owner case\n");

    Dos = (PIMAGE_DOS_HEADER)Data;
    Nt = (PIMAGE_NT_HEADERS)(Data + Dos->e_lfanew);
    OriginalMachine = Nt->FileHeader.Machine;
    Nt->FileHeader.Machine = 0x1234;
    MakeTempPath(Path, L"machine.exe");
    WriteWholeFile(Path, Data, g_GuiSize);
    SetRestrictedSecurity(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgWrongMachine, "wrong machine: %u\n", Status);
    DeleteFileW(Path);

    Nt->FileHeader.Machine = OriginalMachine;
    Nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
    MakeTempPath(Path, L"dll.exe");
    WriteWholeFile(Path, Data, g_GuiSize);
    SetRestrictedSecurity(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotGuiExe, "dll: %u\n", Status);
    DeleteFileW(Path);
    Nt->FileHeader.Characteristics &= ~IMAGE_FILE_DLL;

    Nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    MakeTempPath(Path, L"cui.exe");
    WriteWholeFile(Path, Data, g_GuiSize);
    SetRestrictedSecurity(Path);
    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgNotGuiExe, "cui: %u\n", Status);
    DeleteFileW(Path);

    MakeTempPath(Path, L"gui.exe");
    DeleteFileW(Path);
    HeapFree(GetProcessHeap(), 0, Data);
}

static void
Test_DirectoryAndLinks(void)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH], Link[MAX_PATH], Via[MAX_PATH], Sym[MAX_PATH];
    HANDLE h;
    WLIMG_STATUS Status;

    if (g_Gui == NULL)
    {
        skip("no GUI fixture\n");
        return;
    }

    MakeTempPath(Dir, L"wlimg-dir");
    CreateDirectoryW(Dir, NULL);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\gui.exe", Dir);
    WriteWholeFile(Path, g_Gui, g_GuiSize);

    if (!SetRestrictedSecurity(Path) || !SetRestrictedSecurity(Dir))
    {
        skip("cannot set owner/DACL on fixtures (not elevated?)\n");
        DeleteFileW(Path);
        RemoveDirectoryW(Dir);
        return;
    }

    Status = WlImgOpenVerified(Path, WLIMG_CHECK_DIRECTORY, &h);
    ok(Status == WlImgOk, "restricted dir: %u\n", Status);
    if (Status == WlImgOk)
        CloseHandle(h);

    /* R4b: the same file reached through a junction ancestor is not the path given. */
    MakeTempPath(Link, L"wlimg-link");
    if (MakeJunction(Link, Dir))
    {
        StringCchPrintfW(Via, ARRAYSIZE(Via), L"%s\\gui.exe", Link);
        Status = WlImgOpenVerified(Via, 0, &h);
        ok(Status == WlImgBadPath, "via junction: %u\n", Status);
        ok(h == INVALID_HANDLE_VALUE, "via junction: handle %p\n", h);
        RemoveDirectoryW(Link);
    }
    else
        skip("cannot create a junction (%lu)\n", GetLastError());

    /* R4b: a file symlink is a reparse point and is refused as well. */
    MakeTempPath(Sym, L"wlimg-sym.exe");
    if (CreateSymbolicLinkW(Sym, Path, 0))
    {
        Status = WlImgOpenVerified(Sym, 0, &h);
        ok(Status == WlImgBadPath, "symlink: %u\n", Status);
        DeleteFileW(Sym);
    }
    else
        skip("cannot create a file symlink (%lu)\n", GetLastError());

    ok(GrantUsersWrite(Dir), "grant users write on dir\n");
    Status = WlImgOpenVerified(Path, WLIMG_CHECK_DIRECTORY, &h);
    ok(Status == WlImgDirectoryWritable, "users-writable dir: %u\n", Status);

    Status = WlImgOpenVerified(Path, 0, &h);
    ok(Status == WlImgOk, "file only: %u\n", Status);
    if (Status == WlImgOk)
        CloseHandle(h);

    SetRestrictedSecurity(Dir);
    DeleteFileW(Path);
    RemoveDirectoryW(Dir);
}

START_TEST(ImageCheck)
{
    WCHAR Base[MAX_PATH], Src[MAX_PATH];

    GetTempPathW(ARRAYSIZE(Base), Base);
    StringCchPrintfW(g_TempDir, ARRAYSIZE(g_TempDir), L"%swlimgcheck-%lu", Base, GetCurrentProcessId());
    CreateDirectoryW(g_TempDir, NULL);

    GetSystemDirectoryW(Base, ARRAYSIZE(Base));
    StringCchPrintfW(Src, ARRAYSIZE(Src), L"%s\\notepad.exe", Base);
    g_Gui = ReadWholeFile(Src, &g_GuiSize);

    ok(wcscmp(WlImgStatusName(WlImgOk), L"ok") == 0, "status name\n");

    Test_PathRules();
    Test_System32Deployment();
    Test_PeRules();
    Test_DirectoryAndLinks();

    if (g_Gui)
        HeapFree(GetProcessHeap(), 0, g_Gui);
    RemoveDirectoryW(g_TempDir);
}
