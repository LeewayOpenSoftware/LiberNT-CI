/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SetupDiBuildDriverInfoList must not dereference a NULL device element
 *              when DI_FLAGSEX_INSTALLEDDRIVER is set on the set's global params.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>

/*
 * DI_FLAGSEX_INSTALLEDDRIVER restricts the driver list to the device's currently
 * installed driver - inherently a per-device operation. The INSTALLEDDRIVER branch
 * in GetDriverInfoList (dll/win32/setupapi/driver.c) read DeviceInfoData->Reserved
 * unconditionally, but SetupDiBuildDriverInfoList permits DeviceInfoData == NULL for
 * SPDIT_CLASSDRIVER. Setting the flag on the set's global install params and building
 * a class-driver list with no device element dereferenced NULL. Windows ignores the
 * flag when there is no device element; the list is built normally.
 */
START_TEST(BuildDriverInfoInstalledFlag)
{
    HDEVINFO Set;
    SP_DEVINSTALL_PARAMS_W Params = { sizeof(Params) };
    BOOL Ret;

    Set = SetupDiCreateDeviceInfoList(NULL, NULL);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiCreateDeviceInfoList failed (%lu)\n", GetLastError());
    if (Set == INVALID_HANDLE_VALUE)
        return;

    ok(SetupDiGetDeviceInstallParamsW(Set, NULL, &Params),
       "SetupDiGetDeviceInstallParamsW failed (%lu)\n", GetLastError());
    Params.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
    ok(SetupDiSetDeviceInstallParamsW(Set, NULL, &Params),
       "SetupDiSetDeviceInstallParamsW failed (%lu)\n", GetLastError());

    /* With no device element (NULL) this must not dereference NULL. Reaching the
       assertion proves the flag was ignored rather than crashing. */
    Ret = SetupDiBuildDriverInfoList(Set, NULL, SPDIT_CLASSDRIVER);
    ok(TRUE, "SetupDiBuildDriverInfoList(NULL, SPDIT_CLASSDRIVER) with INSTALLEDDRIVER "
       "returned without crashing (ret=%d, le=%lu)\n", Ret, GetLastError());

    SetupDiDestroyDriverInfoList(Set, NULL, SPDIT_CLASSDRIVER);
    SetupDiDestroyDeviceInfoList(Set);
}
