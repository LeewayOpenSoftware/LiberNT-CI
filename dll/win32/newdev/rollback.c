/*
 * PROJECT:     LiberNT New Device Installer
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DiRollbackDriver public entry point
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "newdev_private.h"

#include <cfgmgr32.h>
#include <umpnpmgr/drvinst.h>

BOOL WINAPI
DiRollbackDriver(
    IN HDEVINFO DeviceInfoSet,
    IN PSP_DEVINFO_DATA DeviceInfoData,
    IN HWND hwndParent OPTIONAL,
    IN DWORD Flags,
    OUT PBOOL NeedReboot OPTIONAL)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    BOOL RestartRequired = FALSE;
    DWORD Error;

    TRACE("DiRollbackDriver(%p %p %p 0x%lx %p)\n",
          DeviceInfoSet, DeviceInfoData, hwndParent, Flags, NeedReboot);

    if (NeedReboot)
        *NeedReboot = FALSE;

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE || !DeviceInfoData)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Flags & ~ROLLBACK_BITS)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    if (!IsUserAdmin())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, DeviceInfoData, InstanceId,
                                     RTL_NUMBER_OF(InstanceId), NULL))
        return FALSE;

    Error = DrvInstRollbackEx(InstanceId, &RestartRequired);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return FALSE;
    }

    if (NeedReboot)
        *NeedReboot = RestartRequired;
    return TRUE;
}
