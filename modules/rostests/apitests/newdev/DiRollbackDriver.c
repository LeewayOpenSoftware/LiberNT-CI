/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the DiRollbackDriver entry point
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winreg.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#define CHECK_INVALID_PARAMETER(Call, NeedReboot) \
    do \
    { \
        BOOL Result_; \
        DWORD Error_; \
        *(NeedReboot) = TRUE; \
        SetLastError(0xdeadbeef); \
        Result_ = (Call); \
        Error_ = GetLastError(); \
        ok(!Result_, "%s succeeded\n", #Call); \
        ok(Error_ == ERROR_INVALID_PARAMETER, "%s set error %lu\n", #Call, Error_); \
        ok(*(NeedReboot) == FALSE, "%s touched NeedReboot on failure\n", #Call); \
    } while (0)

START_TEST(DiRollbackDriver)
{
    SP_DEVINFO_DATA DeviceInfoData = { sizeof(DeviceInfoData) };
    HDEVINFO Set;
    BOOL Reboot;

    Set = SetupDiCreateDeviceInfoList(NULL, NULL);
    ok(Set != NULL && Set != INVALID_HANDLE_VALUE,
       "SetupDiCreateDeviceInfoList failed with error %lu\n", GetLastError());
    if (Set == NULL || Set == INVALID_HANDLE_VALUE)
        return;

    ok(SetupDiCreateDeviceInfoW(Set, L"Root\\TramoB\\0000", &GUID_DEVCLASS_UNKNOWN,
                                NULL, 0, DICD_GENERATE_ID, &DeviceInfoData),
       "SetupDiCreateDeviceInfoW failed with error %lu\n", GetLastError());

    CHECK_INVALID_PARAMETER(DiRollbackDriver(NULL, &DeviceInfoData, NULL, 0, &Reboot), &Reboot);
    CHECK_INVALID_PARAMETER(DiRollbackDriver(Set, NULL, NULL, 0, &Reboot), &Reboot);
    CHECK_INVALID_PARAMETER(DiRollbackDriver(INVALID_HANDLE_VALUE, &DeviceInfoData, NULL, 0, &Reboot), &Reboot);

    Reboot = TRUE;
    SetLastError(0xdeadbeef);
    ok(!DiRollbackDriver(Set, &DeviceInfoData, NULL, 0x2, &Reboot),
       "DiRollbackDriver accepted a flag outside ROLLBACK_BITS\n");
    ok(GetLastError() == ERROR_INVALID_FLAGS,
       "DiRollbackDriver set error %lu, expected ERROR_INVALID_FLAGS\n", GetLastError());
    ok(Reboot == FALSE, "DiRollbackDriver touched NeedReboot on failure\n");

    if (!IsUserAdmin())
    {
        Reboot = TRUE;
        SetLastError(0xdeadbeef);
        ok(!DiRollbackDriver(Set, &DeviceInfoData, NULL, 0, &Reboot),
           "DiRollbackDriver succeeded for a non-administrator\n");
        ok(GetLastError() == ERROR_ACCESS_DENIED,
           "DiRollbackDriver set error %lu, expected ERROR_ACCESS_DENIED\n", GetLastError());
        ok(Reboot == FALSE, "DiRollbackDriver touched NeedReboot on failure\n");
    }
    else
    {
        win_skip("Non-administrator checks require a restricted token\n");
    }

    SetupDiDestroyDeviceInfoList(Set);
}
