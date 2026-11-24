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

/* FunctionCodes whose request carries a data block, per the
 * NETLOGON_CONTROL_DATA_INFORMATION switch cases in netlogon.idl. */
static BOOL
NetpLogonControlNeedsData(_In_ DWORD FunctionCode)
{
    switch (FunctionCode)
    {
        case NETLOGON_CONTROL_REDISCOVER:  /* 5     -> TrustedDomainName */
        case NETLOGON_CONTROL_TC_QUERY:    /* 6     -> TrustedDomainName */
        case 8:                            /* FIND_USER        -> UserName */
        case 9:                            /* CHANGE_PASSWORD  -> TrustedDomainName */
        case 10:                           /* TC_VERIFY        -> TrustedDomainName */
        case NETLOGON_CONTROL_SET_DBFLAG:  /* 65534 -> DebugFlag */
            return TRUE;

        default:
            return FALSE;
    }
}

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
    NET_API_STATUS status;

    TRACE("I_NetLogonControl2(%s, %lu, %lu, %p, %p)\n",
          debugstr_w(ServerName), FunctionCode, QueryLevel, Data, Buffer);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    *Buffer = NULL;

    if (QueryLevel < 1 || QueryLevel > 4)
        return ERROR_INVALID_LEVEL;

    if (Data == NULL && NetpLogonControlNeedsData(FunctionCode))
        return ERROR_INVALID_PARAMETER;

    QueryInfo.NetlogonInfo1 = NULL;

    RpcTryExcept
    {
        status = NetrLogonControl2((LOGONSRV_HANDLE)ServerName,
                                   FunctionCode,
                                   QueryLevel,
                                   (PNETLOGON_CONTROL_DATA_INFORMATION)Data,
                                   &QueryInfo);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        status = I_RpcMapWin32Status(RpcExceptionCode());
    }
    RpcEndExcept;

    /* Every query level shares the union's pointer slot; the server allocated the
     * NETLOGON_INFO_* block, which the caller releases with NetApiBufferFree. */
    if (status == NERR_Success)
        *Buffer = (PBYTE)QueryInfo.NetlogonInfo1;

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

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    *Buffer = NULL;

    if (QueryLevel < 1 || QueryLevel > 4)
        return ERROR_INVALID_LEVEL;

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
        *Buffer = (PBYTE)QueryInfo.NetlogonInfo1;

    return status;
}
