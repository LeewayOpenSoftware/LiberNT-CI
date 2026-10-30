/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the driver package signature verification (M6)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define UNICODE
#define _UNICODE

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winreg.h>
#include <stdio.h>
#include <setupapi.h>

#define PACKAGE_SIG_UNSIGNED        0
#define PACKAGE_SIG_SIGNED_VALID    1
#define PACKAGE_SIG_HASH_MISMATCH   2
#define PACKAGE_SIG_CATALOG_INVALID 3

typedef DWORD (WINAPI *PVERIFY_PACKAGE)(PCWSTR, PDWORD, PWSTR, DWORD);

#define IDR_GOODCAT 1
#define IDR_BADCAT  2

static const WCHAR TestSysContents[] = L"LnPkg fake driver binary\n";

static const WCHAR TestInfWithCatalog[] =
    L"[Version]\r\n"
    L"Signature=\"$Windows NT$\"\r\n"
    L"Class=System\r\n"
    L"ClassGuid={4d36e97d-e325-11ce-bfc1-08002be10318}\r\n"
    L"Provider=%Provider%\r\n"
    L"DriverVer=01/01/2026,1.0.0.0\r\n"
    L"CatalogFile=lnpkg.cat\r\n"
    L"\r\n"
    L"[SourceDisksNames]\r\n"
    L"1 = %DiskName%\r\n"
    L"\r\n"
    L"[SourceDisksFiles]\r\n"
    L"lnpkg.sys = 1\r\n"
    L"\r\n"
    L"[DestinationDirs]\r\n"
    L"LnPkg.CopyFiles = 12\r\n"
    L"\r\n"
    L"[LnPkgInst]\r\n"
    L"CopyFiles = LnPkg.CopyFiles\r\n"
    L"\r\n"
    L"[LnPkg.CopyFiles]\r\n"
    L"lnpkg.sys\r\n"
    L"\r\n"
    L"[Strings]\r\n"
    L"Provider = \"Leeway Open Software\"\r\n"
    L"DiskName = \"LnPkg test disk\"\r\n";

static const WCHAR TestInfNoCatalog[] =
    L"[Version]\r\n"
    L"Signature=\"$Windows NT$\"\r\n"
    L"Class=System\r\n"
    L"ClassGuid={4d36e97d-e325-11ce-bfc1-08002be10318}\r\n"
    L"Provider=%Provider%\r\n"
    L"DriverVer=01/01/2026,1.0.0.0\r\n"
    L"\r\n"
    L"[SourceDisksNames]\r\n"
    L"1 = %DiskName%\r\n"
    L"\r\n"
    L"[SourceDisksFiles]\r\n"
    L"lnpkg.sys = 1\r\n"
    L"\r\n"
    L"[DestinationDirs]\r\n"
    L"LnPkg.CopyFiles = 12\r\n"
    L"\r\n"
    L"[LnPkgInst]\r\n"
    L"CopyFiles = LnPkg.CopyFiles\r\n"
    L"\r\n"
    L"[LnPkg.CopyFiles]\r\n"
    L"lnpkg.sys\r\n"
    L"\r\n"
    L"[Strings]\r\n"
    L"Provider = \"Leeway Open Software\"\r\n"
    L"DiskName = \"LnPkg test disk\"\r\n";

static BOOL
WriteWholeFile(PCWSTR Path, PCWSTR Contents, DWORD ContentsBytes, BOOL InfBom)
{
    HANDLE hFile;
    DWORD Written;
    WORD Bom = 0xFEFF;
    BOOL Result;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    Result = TRUE;
    if (InfBom)
        Result = WriteFile(hFile, &Bom, sizeof(Bom), &Written, NULL);
    if (Result)
        Result = WriteFile(hFile, Contents, ContentsBytes, &Written, NULL);

    CloseHandle(hFile);
    return Result;
}

static BOOL
WriteBytes(PCWSTR Path, const VOID *Data, DWORD Size)
{
    HANDLE hFile;
    DWORD Written;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!WriteFile(hFile, Data, Size, &Written, NULL))
    {
        CloseHandle(hFile);
        return FALSE;
    }
    CloseHandle(hFile);
    return Written == Size;
}

static BOOL
ExtractResource(INT Id, PCWSTR Path)
{
    HRSRC Resource;
    HGLOBAL Loaded;
    LPVOID Data;
    DWORD Size;

    Resource = FindResourceW(NULL, MAKEINTRESOURCEW(Id), RT_RCDATA);
    if (!Resource)
        return FALSE;
    Loaded = LoadResource(NULL, Resource);
    if (!Loaded)
        return FALSE;
    Data = LockResource(Loaded);
    Size = SizeofResource(NULL, Resource);
    if (!Data || !Size)
        return FALSE;
    return WriteBytes(Path, Data, Size);
}

START_TEST(PackageSignature)
{
    WCHAR TempDir[MAX_PATH], TestDir[MAX_PATH];
    WCHAR InfPath[MAX_PATH], SysPath[MAX_PATH], CatPath[MAX_PATH];
    DWORD SigState;
    WCHAR Mismatch[MAX_PATH];
    PVERIFY_PACKAGE Verify;
    HMODULE Module;

    Module = GetModuleHandleW(L"setupapi");
    Verify = (PVERIFY_PACKAGE)GetProcAddress(Module, "LnSetupVerifyDriverPackage");
    if (!Verify)
    {
        win_skip("LnSetupVerifyDriverPackage is LiberNT-only\n");
        return;
    }

    if (GetTempPathW(RTL_NUMBER_OF(TempDir), TempDir) == 0)
        return;
    _snwprintf(TestDir, RTL_NUMBER_OF(TestDir), L"%slnpkgsigntest", TempDir);
    if (!CreateDirectoryW(TestDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return;

    _snwprintf(InfPath, RTL_NUMBER_OF(InfPath), L"%s\\lnpkg.inf", TestDir);
    _snwprintf(SysPath, RTL_NUMBER_OF(SysPath), L"%s\\lnpkg.sys", TestDir);
    _snwprintf(CatPath, RTL_NUMBER_OF(CatPath), L"%s\\lnpkg.cat", TestDir);

    ok(WriteWholeFile(InfPath, TestInfWithCatalog,
                      sizeof(TestInfWithCatalog) - sizeof(WCHAR), TRUE),
       "writing %ls failed\n", InfPath);
    ok(WriteWholeFile(SysPath, TestSysContents,
                      sizeof(TestSysContents) - sizeof(WCHAR), FALSE),
       "writing %ls failed\n", SysPath);

    /* Signed: the catalog was generated for exactly these file bytes. */
    ok(ExtractResource(IDR_GOODCAT, CatPath), "extracting the good catalog failed\n");
    SigState = 0xFFFFFFFF;
    Mismatch[0] = UNICODE_NULL;
    ok(Verify(InfPath, &SigState, Mismatch, RTL_NUMBER_OF(Mismatch)) == ERROR_SUCCESS &&
       SigState == PACKAGE_SIG_SIGNED_VALID,
       "good package: expected state 1, got %lu\n", SigState);

    /* Hash mismatch: one byte of the binary changed. */
    ok(WriteBytes(SysPath, "x", 1), "altering %ls failed\n", SysPath);
    SigState = 0xFFFFFFFF;
    Mismatch[0] = UNICODE_NULL;
    ok(Verify(InfPath, &SigState, Mismatch, RTL_NUMBER_OF(Mismatch)) == ERROR_SUCCESS &&
       SigState == PACKAGE_SIG_HASH_MISMATCH,
       "altered binary: expected state 2, got %lu\n", SigState);
    ok(Mismatch[0] != UNICODE_NULL, "no mismatch file was reported\n");

    ok(WriteWholeFile(SysPath, TestSysContents,
                      sizeof(TestSysContents) - sizeof(WCHAR), FALSE),
       "restoring %ls failed\n", SysPath);

    /* Invalid catalog: bytes that are not a catalog at all. */
    ok(ExtractResource(IDR_BADCAT, CatPath), "extracting the bad catalog failed\n");
    SigState = 0xFFFFFFFF;
    ok(Verify(InfPath, &SigState, Mismatch, RTL_NUMBER_OF(Mismatch)) == ERROR_SUCCESS &&
       SigState == PACKAGE_SIG_CATALOG_INVALID,
       "invalid catalog: expected state 3, got %lu\n", SigState);

    /* Unsigned: an INF with no CatalogFile entry. */
    ok(WriteWholeFile(InfPath, TestInfNoCatalog,
                      sizeof(TestInfNoCatalog) - sizeof(WCHAR), TRUE),
       "writing %ls failed\n", InfPath);
    SigState = 0xFFFFFFFF;
    ok(Verify(InfPath, &SigState, Mismatch, RTL_NUMBER_OF(Mismatch)) == ERROR_SUCCESS &&
       SigState == PACKAGE_SIG_UNSIGNED,
       "no catalog entry: expected state 0, got %lu\n", SigState);

    DeleteFileW(InfPath);
    DeleteFileW(SysPath);
    DeleteFileW(CatPath);
    RemoveDirectoryW(TestDir);
}
