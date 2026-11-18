/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SetupDiBuildDriverInfoList must not crash on a malformed INF whose
 *              [Version] ClassGuid value is longer than the internal GUID buffer.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <stdio.h>
#include <setupapi.h>

/*
 * The [Version] ClassGuid value below is 77 characters - longer than a real GUID
 * string (38) and longer than the internal guidW[MAX_GUID_STRING_LEN + 1] buffer
 * in GetVersionInformationFromInfFile (dll/win32/setupapi/driver.c). A build that
 * passes sizeof(guidW) (bytes) as the *character* count to SetupGetLineTextW tells
 * it there is twice the real room, so it writes this value past the stack buffer
 * and smashes the stack while the driver list is built. With the size passed in
 * characters the value is rejected (ERROR_INSUFFICIENT_BUFFER) and the INF skipped.
 *
 * GetVersionInformationFromInfFile is called for every candidate INF before the
 * class/compatible split in SetupDiBuildDriverInfoList, so the overflow is reachable
 * both through the class-driver list (no device element) and through the compatible-
 * driver list (which requires a device element). Both are exercised here so the
 * pre-fix crash reproduces regardless of which list is built. Reaching the assertion
 * after each call is what proves no overflow happened; the bad INF contributes no driver.
 */
static const char TestInfData[] =
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "Class=LntTest\r\n"
    "ClassGuid={AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA}\r\n"
    "\r\n"
    "[Manufacturer]\r\n"
    "%Mfg%=Models,NTamd64,NTx86\r\n"
    "\r\n"
    "[Models.NTamd64]\r\n"
    "%Dev%=Inst,TEST\\LNT_BADCLASSGUID\r\n"
    "\r\n"
    "[Models.NTx86]\r\n"
    "%Dev%=Inst,TEST\\LNT_BADCLASSGUID\r\n"
    "\r\n"
    "[Inst]\r\n"
    "\r\n"
    "[Strings]\r\n"
    "Mfg=\"LntTest Mfg\"\r\n"
    "Dev=\"LntTest Device\"\r\n";

static const WCHAR HardwareId[] = L"TEST\\LNT_BADCLASSGUID\0";
static const GUID NullClassGuid = { 0 };

static BOOL write_inf(PCWSTR Path)
{
    HANDLE hFile;
    DWORD written = 0;
    BOOL wrote;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    wrote = WriteFile(hFile, TestInfData, (DWORD)(sizeof(TestInfData) - 1), &written, NULL);
    CloseHandle(hFile);
    return wrote && written == sizeof(TestInfData) - 1;
}

/* Point the set's (or the device's) install params at the single bad INF. */
static BOOL set_single_inf(HDEVINFO Set, PSP_DEVINFO_DATA Dev, PCWSTR InfPath)
{
    SP_DEVINSTALL_PARAMS_W Params = { sizeof(Params) };

    if (!SetupDiGetDeviceInstallParamsW(Set, Dev, &Params))
        return FALSE;
    Params.Flags |= DI_ENUMSINGLEINF;
    lstrcpyW(Params.DriverPath, InfPath);
    return SetupDiSetDeviceInstallParamsW(Set, Dev, &Params);
}

START_TEST(BuildDriverInfoBadInf)
{
    WCHAR TempDir[MAX_PATH];
    WCHAR InfPath[MAX_PATH];
    HDEVINFO Set;
    SP_DEVINFO_DATA DevData = { sizeof(DevData) };
    BOOL Ret;

    if (!GetTempPathW(ARRAYSIZE(TempDir), TempDir))
    {
        skip("GetTempPathW failed (%lu)\n", GetLastError());
        return;
    }
    lstrcpyW(InfPath, TempDir);
    lstrcatW(InfPath, L"lnt_badclassguid.inf");

    if (!write_inf(InfPath))
    {
        skip("Could not write the test INF (%lu)\n", GetLastError());
        return;
    }

    Set = SetupDiCreateDeviceInfoList(NULL, NULL);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiCreateDeviceInfoList failed (%lu)\n", GetLastError());
    if (Set == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(InfPath);
        return;
    }

    /* Case 1: class-driver list, no device element. */
    ok(set_single_inf(Set, NULL, InfPath), "set_single_inf(set) failed (%lu)\n", GetLastError());
    Ret = SetupDiBuildDriverInfoList(Set, NULL, SPDIT_CLASSDRIVER);
    ok(TRUE, "SPDIT_CLASSDRIVER returned without crashing (ret=%d, le=%lu)\n", Ret, GetLastError());
    SetupDiDestroyDriverInfoList(Set, NULL, SPDIT_CLASSDRIVER);

    /* Case 2: compatible-driver list, which requires a device element. The same bad
       [Version] is parsed while enumerating the single INF for this device. */
    if (SetupDiCreateDeviceInfoW(Set, L"LNT_BADCLASSGUID", &NullClassGuid, NULL, NULL,
                                 DICD_GENERATE_ID, &DevData))
    {
        SetupDiSetDeviceRegistryPropertyW(Set, &DevData, SPDRP_HARDWAREID,
                                          (const BYTE *)HardwareId, sizeof(HardwareId));
        ok(set_single_inf(Set, &DevData, InfPath), "set_single_inf(dev) failed (%lu)\n", GetLastError());
        Ret = SetupDiBuildDriverInfoList(Set, &DevData, SPDIT_COMPATDRIVER);
        ok(TRUE, "SPDIT_COMPATDRIVER returned without crashing (ret=%d, le=%lu)\n", Ret, GetLastError());
        SetupDiDestroyDriverInfoList(Set, &DevData, SPDIT_COMPATDRIVER);
    }
    else
    {
        skip("SetupDiCreateDeviceInfoW failed (%lu); compatible-driver path not exercised\n",
             GetLastError());
    }

    SetupDiDestroyDeviceInfoList(Set);
    DeleteFileW(InfPath);
}
