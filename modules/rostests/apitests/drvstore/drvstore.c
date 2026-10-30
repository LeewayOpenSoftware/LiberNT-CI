/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the driver store package staging
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

static const WCHAR TestInfContents[] =
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

static const WCHAR TestSysContents[] = L"LnPkg fake driver binary\n";

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
PathExists(PCWSTR Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

START_TEST(DrvStoreStaging)
{
    WCHAR TempDir[MAX_PATH], TestDir[MAX_PATH];
    WCHAR InfPath[MAX_PATH], SysPath[MAX_PATH];
    WCHAR StoreInf[MAX_PATH], Repository[MAX_PATH], PackageDir[MAX_PATH], StoreSys[MAX_PATH];
    WCHAR PublishedPath[MAX_PATH];
    WCHAR RootBuffer[MAX_PATH];
    PWSTR LastSlash;
    DWORD Required, RepoLength;
    BOOL Staged = FALSE;

    if (!IsUserAdmin())
    {
        win_skip("Driver store staging requires an administrator\n");
        return;
    }

    if (GetTempPathW(RTL_NUMBER_OF(TempDir), TempDir) == 0)
        return;
    _snwprintf(TestDir, RTL_NUMBER_OF(TestDir), L"%slnpkgtest", TempDir);
    if (!CreateDirectoryW(TestDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return;

    _snwprintf(InfPath, RTL_NUMBER_OF(InfPath), L"%s\\lnpkg.inf", TestDir);
    _snwprintf(SysPath, RTL_NUMBER_OF(SysPath), L"%s\\lnpkg.sys", TestDir);

    ok(WriteWholeFile(InfPath, TestInfContents,
                      sizeof(TestInfContents) - sizeof(WCHAR), TRUE),
       "writing %ls failed with error %lu\n", InfPath, GetLastError());
    ok(WriteWholeFile(SysPath, TestSysContents,
                      sizeof(TestSysContents) - sizeof(WCHAR), FALSE),
       "writing %ls failed with error %lu\n", SysPath, GetLastError());

    /* Documented path: publish with SetupCopyOEMInfW, resolve the published
     * name with SetupGetInfDriverStoreLocationW, remove with
     * SetupUninstallOEMInfW. */
    Required = 0;
    if (SetupGetInfDriverStoreLocationW(L"lnpkg.inf", NULL, NULL,
                                        StoreInf, RTL_NUMBER_OF(StoreInf),
                                        &Required))
    {
        trace("lnpkg.inf was already published, reusing its store package\n");
    }
    else
    {
        if (!SetupCopyOEMInfW(InfPath, NULL, SPOST_NONE, 0,
                              PublishedPath, RTL_NUMBER_OF(PublishedPath),
                              NULL, NULL))
        {
            ok(0, "SetupCopyOEMInfW failed with error %lu\n", GetLastError());
            goto cleanup;
        }
        LastSlash = wcsrchr(PublishedPath, L'\\');
        if (!LastSlash)
            goto cleanup;
        Required = 0;
        if (!SetupGetInfDriverStoreLocationW(LastSlash + 1, NULL, NULL,
                                             StoreInf, RTL_NUMBER_OF(StoreInf),
                                             &Required))
        {
            ok(0, "SetupGetInfDriverStoreLocationW failed with error %lu\n", GetLastError());
            goto cleanup;
        }
    }
    Staged = TRUE;

    ok(PathExists(StoreInf), "staged package INF %ls does not exist\n", StoreInf);

    /* The store directory is <win>\System32\DriverStore\FileRepository\<pkg> */
    ok(GetWindowsDirectoryW(RootBuffer, RTL_NUMBER_OF(RootBuffer)) != 0,
       "GetWindowsDirectoryW failed with error %lu\n", GetLastError());
    _snwprintf(Repository, RTL_NUMBER_OF(Repository),
               L"%s\\System32\\DriverStore\\FileRepository", RootBuffer);
    RepoLength = lstrlenW(Repository);
    ok(_wcsnicmp(StoreInf, Repository, RepoLength) == 0 && StoreInf[RepoLength] == L'\\',
       "%ls is not under the FileRepository root %ls\n", StoreInf, Repository);

    /* The package binary from [SourceDisksFiles] is staged next to the INF. */
    LastSlash = wcsrchr(StoreInf, L'\\');
    ok(LastSlash != NULL, "%ls has no directory part\n", StoreInf);
    if (!LastSlash)
        goto cleanup;
    lstrcpynW(PackageDir, StoreInf, (INT)(LastSlash - StoreInf) + 1);
    _snwprintf(StoreSys, RTL_NUMBER_OF(StoreSys), L"%s\\lnpkg.sys", PackageDir);
    StoreSys[RTL_NUMBER_OF(StoreSys) - 1] = UNICODE_NULL;
    ok(PathExists(StoreSys),
       "staged package binary %ls does not exist next to %ls\n", StoreSys, StoreInf);

    /* A published-name lookup resolves to the same store location. */
    Required = 0;
    ok(SetupGetInfDriverStoreLocationW(L"lnpkg.inf", NULL, NULL,
                                       PublishedPath, RTL_NUMBER_OF(PublishedPath),
                                       &Required) &&
       wcscmp(PublishedPath, StoreInf) == 0,
       "SetupGetInfDriverStoreLocationW did not resolve to %ls\n", StoreInf);

cleanup:
    if (Staged)
    {
        Required = 0;
        if (SetupGetInfPublishedNameW(StoreInf, PublishedPath,
                                      RTL_NUMBER_OF(PublishedPath), &Required))
        {
            ok(SetupUninstallOEMInfW(PublishedPath, SUOI_FORCEDELETE, NULL),
               "SetupUninstallOEMInfW failed with error %lu\n", GetLastError());
        }
        ok(!PathExists(StoreSys), "package binary %ls survived the delete\n", StoreSys);
    }
    DeleteFileW(InfPath);
    DeleteFileW(SysPath);
    RemoveDirectoryW(TestDir);
}
