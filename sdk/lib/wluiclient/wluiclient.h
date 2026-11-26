/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Client side of the wlui interface for LogonUIHost and
 *              LogonRecovery: connect to winlogon and verify who answers.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <rpc.h>
#include <wlui_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLUI_DEFAULT_ENDPOINT L"wlui"

/* This library defines MIDL_user_allocate/MIDL_user_free for its stubs; a consumer
 * that links another RPC client must not define them a second time. */

typedef struct _WLUI_CONNECTION
{
    RPC_BINDING_HANDLE Binding;
    GUID LaunchId;
} WLUI_CONNECTION, *PWLUI_CONNECTION;

/*
 * Binds to ncalrpc:[Endpoint] and names the server the client expects
 * (ExpectedServerSid, LocalSystem for winlogon) through RPC_SECURITY_QOS_V3:
 * the transport refuses the connection when the endpoint is owned by anyone
 * else, so nothing is sent to an impostor. LaunchId is the value winlogon put
 * on the command line. Returns RPC_S_OK or the RPC error.
 */
RPC_STATUS
NTAPI
WluiConnect(
    _In_opt_ PCWSTR Endpoint,
    _In_ PSID ExpectedServerSid,
    _In_ const GUID *LaunchId,
    _Out_ PWLUI_CONNECTION Connection);

VOID NTAPI WluiDisconnect(_Inout_ PWLUI_CONNECTION Connection);

/* Builds the LocalSystem SID production callers pass as ExpectedServerSid. */
BOOL NTAPI WluiGetLocalSystemSid(_Out_writes_bytes_(SECURITY_MAX_SID_SIZE) PSID Sid);

/* Thin wrappers: each maps an RPC exception to its Win32 status. */
DWORD NTAPI WluiClientReady(_In_ PWLUI_CONNECTION Connection);
DWORD NTAPI WluiClientHeartbeat(_In_ PWLUI_CONNECTION Connection);
DWORD NTAPI WluiClientWaitForCommand(_In_ PWLUI_CONNECTION Connection, _Out_ WLUI_COMMAND *Command);
DWORD NTAPI WluiClientSubmitSerialization(_In_ PWLUI_CONNECTION Connection, _In_ DWORD Scenario,
                                          _In_ const GUID *ProviderClsid, _In_ DWORD AuthenticationPackage,
                                          _In_reads_bytes_(cbSerialization) const BYTE *Serialization,
                                          _In_ DWORD cbSerialization);
DWORD NTAPI WluiClientRequestAction(_In_ PWLUI_CONNECTION Connection, _In_ WLUI_ACTION Action);
DWORD NTAPI WluiClientReportProviderFault(_In_ PWLUI_CONNECTION Connection, _In_ const GUID *ProviderClsid,
                                          _In_ DWORD Reason);
DWORD NTAPI WluiClientExiting(_In_ PWLUI_CONNECTION Connection, _In_ WLUI_EXIT_REASON Reason);

#ifdef __cplusplus
}
#endif
