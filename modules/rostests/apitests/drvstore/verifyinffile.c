/*
 * PROJECT:     LiberNT API tests
 * PURPOSE:     Tests for SetupVerifyInfFileW over the M6 verification
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

#define IDR_FULLCAT 3

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
ExtractResource(INT Id, PCWSTR Path)
{
    HRSRC Resource;
    HGLOBAL Loaded;
    LPVOID Data;
    DWORD Size;
    HANDLE hFile;
    DWORD Written;

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

START_TEST(VerifyInfFile)
{
    WCHAR TempDir[MAX_PATH], TestDir[MAX_PATH];
    WCHAR InfPath[MAX_PATH], SysPath[MAX_PATH], CatPath[MAX_PATH];
    SP_INF_SIGNER_INFO_INF_W Info;

    if (GetTempPathW(RTL_NUMBER_OF(TempDir), TempDir) == 0)
        return;
    _snwprintf(TestDir, RTL_NUMBER_OF(TestDir), L"%slnpkgverifytest", TempDir);
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

    /* The full catalog fixture was generated over exactly these bytes. */
    ok(ExtractResource(IDR_FULLCAT, CatPath), "extracting the full catalog failed\n");

    Info.cbSize = sizeof(Info);
    ok(SetupVerifyInfFileW(InfPath, NULL, &Info),
       "SetupVerifyInfFileW on the signed INF failed with error %lu\n", GetLastError());
    ok(wcscmp(Info.CatalogFile, L"lnpkg.cat") == 0,
       "CatalogFile is '%ls', expected 'lnpkg.cat'\n", Info.CatalogFile);

    /* An altered INF no longer matches the catalog entry. */
    {
        HANDLE hFile = CreateFileW(InfPath, GENERIC_WRITE, 0, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD Written;
        WCHAR Extra = L'X';

        ok(hFile != INVALID_HANDLE_VALUE, "opening %ls failed\n", InfPath);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            SetFilePointer(hFile, 0, NULL, FILE_END);
            WriteFile(hFile, &Extra, sizeof(Extra), &Written, NULL);
            CloseHandle(hFile);
        }
    }
    Info.cbSize = sizeof(Info);
    ok(!SetupVerifyInfFileW(InfPath, NULL, &Info),
       "SetupVerifyInfFileW accepted an altered INF\n");
    ok(GetLastError() == ERROR_INVALID_IMAGE_HASH,
       "altered INF set error %lu, expected %lu\n", GetLastError(), (DWORD)ERROR_INVALID_IMAGE_HASH);

    /* Bad cbSize is rejected. */
    Info.cbSize = sizeof(Info) - 1;
    ok(!SetupVerifyInfFileW(InfPath, NULL, &Info),
       "SetupVerifyInfFileW accepted a bad cbSize\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "bad cbSize set error %lu\n", GetLastError());

    /* No catalog at all: unsigned. */
    DeleteFileW(CatPath);
    Info.cbSize = sizeof(Info);
    ok(!SetupVerifyInfFileW(InfPath, NULL, &Info),
       "SetupVerifyInfFileW accepted an unsigned INF\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND,
       "unsigned INF set error %lu, expected %lu\n", GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);

    DeleteFileW(InfPath);
    DeleteFileW(SysPath);
    DeleteFileW(CatPath);
    RemoveDirectoryW(TestDir);
}
