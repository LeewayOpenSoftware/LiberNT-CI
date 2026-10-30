/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Client side of the driver install jobs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <rpc.h>
#include <rpcdce.h>
#include <umpnpmgr/drvinst.h>
#include <drvinst_c.h>

/* Memory routines the RPC runtime needs for the client stub */
void __RPC_FAR * __RPC_USER
midl_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

void __RPC_USER
midl_user_free(void __RPC_FAR *ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

static DWORD
BindToService(
    _Out_ RPC_BINDING_HANDLE *BindingHandle)
{
    PWSTR StringBinding = NULL;
    RPC_STATUS Status;

    Status = RpcStringBindingComposeW(NULL,
                                      L"ncacn_np",
                                      NULL,
                                      L"\\pipe\\plugplay",
                                      NULL,
                                      &StringBinding);
    if (Status != RPC_S_OK)
        return Status;

    Status = RpcBindingFromStringBindingW(StringBinding, BindingHandle);
    RpcStringFreeW(&StringBinding);

    return Status;
}

DWORD
WINAPI
DrvInstSubmitDevice(
    _In_ PCWSTR DeviceInstanceId,
    _In_ DWORD Flags,
    _Out_ DRVINST_JOB_ID *JobId)
{
    RPC_BINDING_HANDLE Binding;
    DWORD Error;

    if (!DeviceInstanceId || !JobId)
        return ERROR_INVALID_PARAMETER;

    *JobId = 0;

    Error = BindToService(&Binding);
    if (Error != ERROR_SUCCESS)
        return Error;

    RpcTryExcept
    {
        Error = DrvInst_SubmitDevice(Binding, DeviceInstanceId, Flags, JobId);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    RpcBindingFree(&Binding);
    return Error;
}

DWORD
WINAPI
DrvInstGetJob(
    _In_ DRVINST_JOB_ID JobId,
    _Out_ PDRVINST_JOB_INFO Info)
{
    RPC_BINDING_HANDLE Binding;
    DWORD Error;

    if (!Info)
        return ERROR_INVALID_PARAMETER;

    ZeroMemory(Info, sizeof(*Info));
    Info->Size = sizeof(*Info);
    Info->JobId = JobId;

    Error = BindToService(&Binding);
    if (Error != ERROR_SUCCESS)
        return Error;

    RpcTryExcept
    {
        Error = DrvInst_GetJob(Binding,
                               JobId,
                               &Info->State,
                               &Info->Source,
                               &Info->Flags,
                               &Info->Result.Win32,
                               &Info->Result.Problem,
                               &Info->Result.Flags,
                               &Info->SubmitTick,
                               &Info->EndTick,
                               Info->DeviceInstanceId,
                               DRVINST_DEVICE_ID_CCH);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    Info->Result.Size = sizeof(Info->Result);
    RpcBindingFree(&Binding);
    return Error;
}

DWORD
WINAPI
DrvInstEnumJobs(
    _Out_writes_to_(*Count, *Count) DRVINST_JOB_ID *JobIds,
    _Inout_ PDWORD Count)
{
    RPC_BINDING_HANDLE Binding;
    DWORD Error, Returned = 0;

    if (!JobIds || !Count)
        return ERROR_INVALID_PARAMETER;

    Error = BindToService(&Binding);
    if (Error != ERROR_SUCCESS)
        return Error;

    RpcTryExcept
    {
        Error = DrvInst_EnumJobs(Binding, *Count, JobIds, &Returned);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    if (Error == ERROR_SUCCESS)
        *Count = Returned;

    RpcBindingFree(&Binding);
    return Error;
}

DWORD
WINAPI
DrvInstWait(
    _In_ DRVINST_JOB_ID JobId,
    _In_ DWORD TimeoutMs,
    _Out_opt_ PDRVINST_JOB_INFO Info)
{
    DRVINST_JOB_INFO Local;
    DWORD Start = GetTickCount();
    DWORD Error;

    for (;;)
    {
        Error = DrvInstGetJob(JobId, &Local);
        if (Error != ERROR_SUCCESS)
            return Error;

        if (Local.State >= DrvInstJobSucceeded)
        {
            if (Info)
                *Info = Local;
            return ERROR_SUCCESS;
        }

        if (GetTickCount() - Start >= TimeoutMs)
        {
            if (Info)
                *Info = Local;
            return ERROR_TIMEOUT;
        }

        Sleep(250);
    }
}

DWORD
WINAPI
DrvInstRollbackEx(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PBOOL NeedReboot)
{
    RPC_BINDING_HANDLE Binding;
    BOOL RestartRequired = FALSE;
    DWORD Error;

    if (!DeviceInstanceId)
        return ERROR_INVALID_PARAMETER;

    Error = BindToService(&Binding);
    if (Error != ERROR_SUCCESS)
        return Error;

    RpcTryExcept
    {
        Error = DrvInst_Rollback(Binding, DeviceInstanceId, &RestartRequired);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    RpcBindingFree(&Binding);

    if (Error == ERROR_SUCCESS && NeedReboot)
        *NeedReboot = RestartRequired;
    return Error;
}

DWORD
WINAPI
DrvInstRollback(
    _In_ PCWSTR DeviceInstanceId)
{
    return DrvInstRollbackEx(DeviceInstanceId, NULL);
}

DWORD
WINAPI
DrvInstSubmitInf(
    _In_ PCWSTR DeviceInstanceId,
    _In_ PCWSTR InfPath,
    _In_ DWORD Flags,
    _Out_ DRVINST_JOB_ID *JobId)
{
    RPC_BINDING_HANDLE Binding;
    DWORD Error;

    if (!DeviceInstanceId || !InfPath || !JobId)
        return ERROR_INVALID_PARAMETER;

    *JobId = 0;

    Error = BindToService(&Binding);
    if (Error != ERROR_SUCCESS)
        return Error;

    RpcTryExcept
    {
        Error = DrvInst_SubmitInf(Binding, DeviceInstanceId, InfPath, Flags, JobId);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = RpcExceptionCode();
    }
    RpcEndExcept;

    RpcBindingFree(&Binding);
    return Error;
}
