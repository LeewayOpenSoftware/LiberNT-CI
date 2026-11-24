/*
 * PROJECT:     LiberNT netapi32.dll
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     I_NetLogonControl / I_NetLogonControl2 client wrappers over the
 *              netlogon RPC interface. Exported to callers through logoncli.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "netapi32.h"
#include <rpc.h>
#include "netlogon_c.h"

WINE_DEFAULT_DEBUG_CHANNEL(netapi32);

NET_API_STATUS
WINAPI
I_NetLogonControl2(
    _In_opt_ LPCWSTR ServerName,
    _In_ DWORD FunctionCode,
    _In_ DWORD QueryLevel,
    _In_ PBYTE Data,
    _Out_ PBYTE *Buffer)
{
    NETLOGON_CONTROL_QUERY_INFORMATION QueryInfo;
    NETLOGON_CONTROL_DATA_INFORMATION DataInfo;
    NET_API_STATUS status;

    TRACE("I_NetLogonControl2(%s, %lu, %lu, %p, %p)\n",
          debugstr_w(ServerName), FunctionCode, QueryLevel, Data, Buffer);

    /* Win11 range-checks the level client-side (and clears Buffer); everything else
     * is validated server-side, so this is the only check that returns before RPC. */
    if (QueryLevel < 1 || QueryLevel > 4)
    {
        if (Buffer != NULL)
            *Buffer = NULL;
        return ERROR_INVALID_LEVEL;
    }

    /* Data is an embedded pointer in the [ref] union, so NULL is legal: fill the arm
     * only when the caller supplied one, and always pass &DataInfo (keeps F1 fixed). */
    ZeroMemory(&DataInfo, sizeof(DataInfo));
    switch (FunctionCode)
    {
        case NETLOGON_CONTROL_REDISCOVER:  /* 5 */
        case NETLOGON_CONTROL_TC_QUERY:    /* 6 */
        case 9:                            /* CHANGE_PASSWORD */
        case 10:                           /* TC_VERIFY */
            if (Data != NULL)
                DataInfo.TrustedDomainName = *(LPWSTR *)Data;
            break;

        case 8:                            /* FIND_USER */
            if (Data != NULL)
                DataInfo.UserName = *(LPWSTR *)Data;
            break;

        case NETLOGON_CONTROL_SET_DBFLAG:  /* 65534 */
            if (Data != NULL)
                DataInfo.DebugFlag = *(DWORD *)Data;
            break;

        default:
            break;   /* this FunctionCode carries no data; the union stays empty */
    }

    QueryInfo.NetlogonInfo1 = NULL;

    RpcTryExcept
    {
        status = NetrLogonControl2((LOGONSRV_HANDLE)ServerName,
                                   FunctionCode,
                                   QueryLevel,
                                   &DataInfo,
                                   &QueryInfo);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        status = I_RpcMapWin32Status(RpcExceptionCode());
    }
    RpcEndExcept;

    /* A NULL Buffer no longer rejects client-side, so on success free the server block here rather than leak it. */
    if (status == NERR_Success)
    {
        if (Buffer != NULL)
            *Buffer = (PBYTE)QueryInfo.NetlogonInfo1;
        else
            NetApiBufferFree(QueryInfo.NetlogonInfo1);
    }

    return status;
}

NET_API_STATUS
WINAPI
I_NetLogonControl(
    _In_opt_ LPCWSTR ServerName,
    _In_ DWORD FunctionCode,
    _In_ DWORD QueryLevel,
    _Out_ PBYTE *Buffer)
{
    NETLOGON_CONTROL_QUERY_INFORMATION QueryInfo;
    NET_API_STATUS status;

    TRACE("I_NetLogonControl(%s, %lu, %lu, %p)\n",
          debugstr_w(ServerName), FunctionCode, QueryLevel, Buffer);

    /* The old API does no client-side validation, not even the level range:
     * everything is decided server-side over RPC. Buffer is written only on success. */
    QueryInfo.NetlogonInfo1 = NULL;

    RpcTryExcept
    {
        status = NetrLogonControl((LOGONSRV_HANDLE)ServerName,
                                  FunctionCode,
                                  QueryLevel,
                                  &QueryInfo);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        status = I_RpcMapWin32Status(RpcExceptionCode());
    }
    RpcEndExcept;

    if (status == NERR_Success)
    {
        if (Buffer != NULL)
            *Buffer = (PBYTE)QueryInfo.NetlogonInfo1;
        else
            NetApiBufferFree(QueryInfo.NetlogonInfo1);
    }

    return status;
}
