/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Client side of the wlui interface (see wluiclient.h).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>
#include <rpc.h>

#include "wluiclient.h"

void __RPC_FAR * __RPC_USER MIDL_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

void __RPC_USER MIDL_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

/* S-1-5-18 laid out by hand: the recovery component links no advapi32. */
BOOL
NTAPI
WluiGetLocalSystemSid(
    _Out_writes_bytes_(SECURITY_MAX_SID_SIZE) PSID Sid)
{
    static const SID LocalSystem = { SID_REVISION, 1, { SECURITY_NT_AUTHORITY }, { SECURITY_LOCAL_SYSTEM_RID } };

    ZeroMemory(Sid, SECURITY_MAX_SID_SIZE);
    CopyMemory(Sid, &LocalSystem, sizeof(LocalSystem));
    return TRUE;
}

/* Structural check only (revision and sub-authority count), without advapi32. */
static BOOL
WluiSidLooksValid(
    _In_opt_ PSID Sid)
{
    const SID *S = Sid;
    return S != NULL && S->Revision == SID_REVISION && S->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES;
}

RPC_STATUS
NTAPI
WluiConnect(
    _In_opt_ PCWSTR Endpoint,
    _In_ PSID ExpectedServerSid,
    _In_ const GUID *LaunchId,
    _Out_ PWLUI_CONNECTION Connection)
{
    RPC_WSTR StringBinding = NULL;
    RPC_SECURITY_QOS_V3_W Qos;
    RPC_STATUS Status;

    ZeroMemory(Connection, sizeof(*Connection));

    if (!WluiSidLooksValid(ExpectedServerSid) || LaunchId == NULL)
        return RPC_S_INVALID_ARG;

    /* A data reference pins the generated client stubs into every link: a binary that defines
     * the WluiXxx server routines itself then fails to link instead of calling them in-process. */
    if (wlui_v1_0_c_ifspec == NULL)
        return RPC_S_UNKNOWN_IF;

    Status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL,
                                      (RPC_WSTR)(Endpoint ? Endpoint : WLUI_DEFAULT_ENDPOINT),
                                      NULL, &StringBinding);
    if (Status != RPC_S_OK)
        return Status;

    Status = RpcBindingFromStringBindingW(StringBinding, &Connection->Binding);
    RpcStringFreeW(&StringBinding);
    if (Status != RPC_S_OK)
        return Status;

    /* P1: name the server. The QOS is kept without any security package; ncalrpc
     * compares the endpoint owner with Sid at connect and fails with ACCESS_DENIED. */
    ZeroMemory(&Qos, sizeof(Qos));
    Qos.Version = 3;
    Qos.Capabilities = RPC_C_QOS_CAPABILITIES_DEFAULT;
    Qos.IdentityTracking = RPC_C_QOS_IDENTITY_STATIC;
    Qos.ImpersonationType = RPC_C_IMP_LEVEL_IMPERSONATE;
    Qos.Sid = ExpectedServerSid;

    Status = RpcBindingSetAuthInfoExW(Connection->Binding, NULL, RPC_C_AUTHN_LEVEL_NONE,
                                      RPC_C_AUTHN_NONE, NULL, RPC_C_AUTHZ_NONE,
                                      (RPC_SECURITY_QOS *)&Qos);
    if (Status != RPC_S_OK)
    {
        RpcBindingFree(&Connection->Binding);
        Connection->Binding = NULL;
        return Status;
    }

    Connection->LaunchId = *LaunchId;
    return RPC_S_OK;
}

VOID
NTAPI
WluiDisconnect(
    _Inout_ PWLUI_CONNECTION Connection)
{
    if (Connection->Binding != NULL)
        RpcBindingFree(&Connection->Binding);
    SecureZeroMemory(Connection, sizeof(*Connection));
}

#define WLUI_CALL(expr)                                         \
    DWORD Result;                                               \
    RpcTryExcept                                                \
    {                                                           \
        Result = (expr);                                        \
    }                                                           \
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)                        \
    {                                                           \
        Result = RpcExceptionCode();                            \
    }                                                           \
    RpcEndExcept;                                               \
    return Result

DWORD NTAPI WluiClientReady(_In_ PWLUI_CONNECTION C)
{
    WLUI_CALL(WluiReady(C->Binding, C->LaunchId));
}

DWORD NTAPI WluiClientHeartbeat(_In_ PWLUI_CONNECTION C)
{
    WLUI_CALL(WluiHeartbeat(C->Binding, C->LaunchId));
}

DWORD NTAPI WluiClientWaitForCommand(_In_ PWLUI_CONNECTION C, _Out_ WLUI_COMMAND *Command)
{
    ZeroMemory(Command, sizeof(*Command));
    WLUI_CALL(WluiWaitForCommand(C->Binding, C->LaunchId, Command));
}

DWORD NTAPI WluiClientSubmitSerialization(_In_ PWLUI_CONNECTION C, _In_ DWORD Scenario,
                                          _In_ const GUID *ProviderClsid, _In_ DWORD AuthenticationPackage,
                                          _In_reads_bytes_(cbSerialization) const BYTE *Serialization,
                                          _In_ DWORD cbSerialization)
{
    if (ProviderClsid == NULL || (cbSerialization != 0 && Serialization == NULL) ||
        cbSerialization > WLUI_MAX_SERIALIZATION)
    {
        return ERROR_INVALID_PARAMETER;
    }
    WLUI_CALL(WluiSubmitSerialization(C->Binding, C->LaunchId, Scenario, *ProviderClsid,
                                      AuthenticationPackage, cbSerialization, Serialization));
}

DWORD NTAPI WluiClientRequestAction(_In_ PWLUI_CONNECTION C, _In_ WLUI_ACTION Action)
{
    WLUI_CALL(WluiRequestAction(C->Binding, C->LaunchId, Action));
}

DWORD NTAPI WluiClientReportProviderFault(_In_ PWLUI_CONNECTION C, _In_ const GUID *ProviderClsid,
                                          _In_ DWORD Reason)
{
    if (ProviderClsid == NULL)
        return ERROR_INVALID_PARAMETER;
    WLUI_CALL(WluiReportProviderFault(C->Binding, C->LaunchId, *ProviderClsid, Reason));
}

DWORD NTAPI WluiClientExiting(_In_ PWLUI_CONNECTION C, _In_ WLUI_EXIT_REASON Reason)
{
    WLUI_CALL(WluiExiting(C->Binding, C->LaunchId, Reason));
}
