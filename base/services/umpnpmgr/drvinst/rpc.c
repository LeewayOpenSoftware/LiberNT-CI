/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RPC interface of the driver install jobs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Is the caller of the current RPC call an administrator? */
static BOOL
IsCallerAdministrator(
    _In_ handle_t hBinding)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID pAdminSid = NULL;
    HANDLE hToken;
    BOOL IsAdmin = FALSE;

    if (!AllocateAndInitializeSid(&NtAuthority, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &pAdminSid))
    {
        return FALSE;
    }

    if (RpcImpersonateClient(hBinding) == RPC_S_OK)
    {
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &hToken))
        {
            if (!CheckTokenMembership(hToken, pAdminSid, &IsAdmin))
                IsAdmin = FALSE;
            CloseHandle(hToken);
        }
        RpcRevertToSelf();
    }

    FreeSid(pAdminSid);
    return IsAdmin;
}


DWORD
__stdcall
DrvInst_SubmitDevice(
    _In_ handle_t hBinding,
    _In_ LPCWSTR pszDeviceInstanceId,
    _In_ DWORD ulFlags,
    _Out_ unsigned __int64 *pullJobId)
{
    DRVINST_JOB_ID JobId = 0;
    DWORD Error;

    *pullJobId = 0;

    if (!IsCallerAdministrator(hBinding))
        return ERROR_ACCESS_DENIED;

    if (ulFlags & ~DRVINST_F_SILENT)
        return ERROR_INVALID_FLAGS;

    Error = DrvInstSubmitDeviceJob(pszDeviceInstanceId, DRVINST_SOURCE_USER, ulFlags, &JobId);
    if (Error == ERROR_SUCCESS)
        *pullJobId = JobId;

    return Error;
}


DWORD
__stdcall
DrvInst_GetJob(
    _In_ handle_t hBinding,
    _In_ unsigned __int64 ullJobId,
    _Out_ DWORD *pulState,
    _Out_ DWORD *pulSource,
    _Out_ DWORD *pulFlags,
    _Out_ DWORD *pulWin32,
    _Out_ DWORD *pulProblem,
    _Out_ DWORD *pulResultFlags,
    _Out_ DWORD *pulSubmitTick,
    _Out_ DWORD *pulEndTick,
    _Out_ WCHAR *pszDeviceInstanceId,
    _In_ DWORD ulDeviceCch)
{
    DRVINST_JOB_INFO Info;

    UNREFERENCED_PARAMETER(hBinding);

    if (ulDeviceCch)
        pszDeviceInstanceId[0] = UNICODE_NULL;

    if (!DrvInstGetJobInfo(ullJobId, &Info))
        return ERROR_NOT_FOUND;

    *pulState = Info.State;
    *pulSource = Info.Source;
    *pulFlags = Info.Flags;
    *pulWin32 = Info.Result.Win32;
    *pulProblem = Info.Result.Problem;
    *pulResultFlags = Info.Result.Flags;
    *pulSubmitTick = Info.SubmitTick;
    *pulEndTick = Info.EndTick;
    if (ulDeviceCch)
        lstrcpynW(pszDeviceInstanceId, Info.DeviceInstanceId, ulDeviceCch);

    return ERROR_SUCCESS;
}


DWORD
__stdcall
DrvInst_EnumJobs(
    _In_ handle_t hBinding,
    _In_ DWORD ulMax,
    _Out_ unsigned __int64 *pullJobIds,
    _Out_ DWORD *pulCount)
{
    PDRVINST_JOB_INFO Jobs;
    ULONG Count, i;

    UNREFERENCED_PARAMETER(hBinding);

    *pulCount = 0;

    if (ulMax == 0)
        return ERROR_SUCCESS;
    if (ulMax > 256)
        return ERROR_INVALID_PARAMETER;

    Jobs = HeapAlloc(GetProcessHeap(), 0, ulMax * sizeof(*Jobs));
    if (!Jobs)
        return ERROR_NOT_ENOUGH_MEMORY;

    Count = DrvInstpEnumJobs(Jobs, ulMax);
    for (i = 0; i < Count; i++)
        pullJobIds[i] = Jobs[i].JobId;
    *pulCount = Count;

    HeapFree(GetProcessHeap(), 0, Jobs);
    return ERROR_SUCCESS;
}


DWORD
__stdcall
DrvInst_Rollback(
    _In_ handle_t hBinding,
    _In_ LPCWSTR pszDeviceInstanceId,
    _Out_ BOOL *pbNeedReboot)
{
    DWORD Error;

    *pbNeedReboot = FALSE;

    if (!IsCallerAdministrator(hBinding))
        return ERROR_ACCESS_DENIED;

    if (!pszDeviceInstanceId || !pszDeviceInstanceId[0] ||
        lstrlenW(pszDeviceInstanceId) >= DRVINST_DEVICE_ID_CCH)
        return ERROR_INVALID_PARAMETER;

    /* Never roll back in the middle of an install job for any device. */
    Error = DrvInstAcquireInstallLock(60000);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = DrvInstRollbackDevice(pszDeviceInstanceId, pbNeedReboot);
    DrvInstReleaseInstallLock();

    return Error;
}


DWORD
__stdcall
DrvInst_SubmitInf(
    _In_ handle_t hBinding,
    _In_ LPCWSTR pszDeviceInstanceId,
    _In_ LPCWSTR pszInfPath,
    _In_ DWORD ulFlags,
    _Out_ unsigned __int64 *pullJobId)
{
    DRVINST_JOB_ID JobId = 0;
    DWORD Error;

    *pullJobId = 0;

    if (!IsCallerAdministrator(hBinding))
        return ERROR_ACCESS_DENIED;

    if (ulFlags & ~DRVINST_F_SILENT)
        return ERROR_INVALID_FLAGS;

    if (!pszDeviceInstanceId || !pszDeviceInstanceId[0] ||
        lstrlenW(pszDeviceInstanceId) >= DRVINST_DEVICE_ID_CCH ||
        !pszInfPath || !pszInfPath[0] || lstrlenW(pszInfPath) >= MAX_PATH ||
        !IsValidInfPath(pszInfPath))
        return ERROR_INVALID_PARAMETER;

    Error = DrvInstSubmitInfJob(pszDeviceInstanceId, pszInfPath,
                                DRVINST_SOURCE_USER, ulFlags, &JobId);
    if (Error == ERROR_SUCCESS)
        *pullJobId = JobId;

    return Error;
}
