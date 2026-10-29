/*
 * PROJECT:     LiberNT Configuration Manager Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver store package management client
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "rpc_private.h"

#include <cfgmgr32.h>
#include <pnp_c.h>

#define CM_DRVSTORE_PUBLISHED_NAME_CCH  64

/* Maps the Win32-style errors of the server call to CONFIGRET codes. */
static CONFIGRET
MapStoreRpcError(
    _In_ DWORD Error)
{
    switch (Error)
    {
        case ERROR_SUCCESS:
            return CR_SUCCESS;
        case ERROR_ACCESS_DENIED:
            return CR_ACCESS_DENIED;
        case ERROR_FILE_NOT_FOUND:
            return CR_INVALID_DATA;
        case ERROR_INVALID_PARAMETER:
            return CR_INVALID_DATA;
        case ERROR_INVALID_FLAGS:
            return CR_INVALID_FLAG;
        default:
            return CR_FAILURE;
    }
}

/***********************************************************************
 * CM_Add_Driver_PackageW
 *
 * Add a driver package to the driver store. Stages the package into the
 * FileRepository and publishes it as an oemNN.inf. Requires an
 * administrator. Only the local machine is supported.
 */
/* LiberNT-private assumed contract: no public prototype exists; the shape
 * follows the cfgmgr32 conventions and only public documentation was used. */
CONFIGRET
WINAPI
CM_Add_Driver_PackageW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ ULONG Flags,
    _In_ PVOID Reserved)
{
    RPC_BINDING_HANDLE BindingHandle;
    WCHAR PublishedName[CM_DRVSTORE_PUBLISHED_NAME_CCH];
    DWORD Error;

    if (Flags != 0)
        return CR_INVALID_FLAG;
    if (Reserved != NULL)
        return CR_INVALID_POINTER;
    if (!DriverPackageInfPath || !DriverPackageInfPath[0])
        return CR_INVALID_DATA;

    if (!PnpGetLocalHandles(&BindingHandle))
        return CR_FAILURE;

    RpcTryExcept
    {
        Error = PNP_DriverStoreAddDriverPackage(BindingHandle,
                                                DriverPackageInfPath,
                                                PublishedName,
                                                CM_DRVSTORE_PUBLISHED_NAME_CCH);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    return MapStoreRpcError(Error);
}

/***********************************************************************
 * CM_Add_Driver_Package_ExW
 */
CONFIGRET
WINAPI
CM_Add_Driver_Package_ExW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ ULONG Flags,
    _In_ PVOID Reserved,
    _In_ HMACHINE hMachine)
{
    if (hMachine != NULL)
        return CR_INVALID_MACHINENAME;

    return CM_Add_Driver_PackageW(DriverPackageInfPath, Flags, Reserved);
}

/***********************************************************************
 * CM_Delete_Driver_PackageW
 *
 * Delete a driver package from the driver store by its published oemNN.inf
 * name or by a path to the INF. Requires an administrator. Only the local
 * machine is supported.
 */
/* LiberNT-private assumed contract: no public prototype exists; the shape
 * follows the cfgmgr32 conventions and only public documentation was used. */
CONFIGRET
WINAPI
CM_Delete_Driver_PackageW(
    _In_ PCWSTR DriverPackageInfPathOrPublishedName,
    _In_ ULONG Flags,
    _In_ PVOID Reserved)
{
    RPC_BINDING_HANDLE BindingHandle;
    WCHAR FullPath[MAX_PATH];
    UINT Length;
    DWORD Error;

    if (Flags != 0)
        return CR_INVALID_FLAG;
    if (Reserved != NULL)
        return CR_INVALID_POINTER;
    if (!DriverPackageInfPathOrPublishedName || !DriverPackageInfPathOrPublishedName[0])
        return CR_INVALID_DATA;

    /* The server takes a full path; resolve a bare oemNN.inf against the INF directory */
    if (!wcschr(DriverPackageInfPathOrPublishedName, L'\\'))
    {
        Length = GetSystemWindowsDirectoryW(FullPath, RTL_NUMBER_OF(FullPath));
        if (Length == 0 || Length + 6 >= RTL_NUMBER_OF(FullPath))
            return CR_INVALID_DATA;
        lstrcatW(FullPath, L"\\inf\\");
        if (lstrlenW(FullPath) + lstrlenW(DriverPackageInfPathOrPublishedName) >= MAX_PATH)
            return CR_INVALID_DATA;
        lstrcatW(FullPath, DriverPackageInfPathOrPublishedName);
        DriverPackageInfPathOrPublishedName = FullPath;
    }

    if (!PnpGetLocalHandles(&BindingHandle))
        return CR_FAILURE;

    RpcTryExcept
    {
        Error = PNP_DriverStoreDeleteDriverPackage(BindingHandle,
                                                   DriverPackageInfPathOrPublishedName);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    return MapStoreRpcError(Error);
}

/***********************************************************************
 * CM_Delete_Driver_Package_ExW
 */
CONFIGRET
WINAPI
CM_Delete_Driver_Package_ExW(
    _In_ PCWSTR DriverPackageInfPathOrPublishedName,
    _In_ ULONG Flags,
    _In_ PVOID Reserved,
    _In_ HMACHINE hMachine)
{
    if (hMachine != NULL)
        return CR_INVALID_MACHINENAME;

    return CM_Delete_Driver_PackageW(DriverPackageInfPathOrPublishedName, Flags, Reserved);
}
