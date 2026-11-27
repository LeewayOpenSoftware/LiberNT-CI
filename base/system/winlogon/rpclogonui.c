/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     wlui server: the channel between winlogon and the processes it
 *              launches on the Winlogon desktop (LogonUIHost, LogonRecovery).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#include <rpc.h>
#include <sddl.h>
#include <wlui_s.h>

#define WLUI_COMMAND_WAIT_MS 30000

static WLUI_LAUNCH_TABLE g_LaunchTable;
static CRITICAL_SECTION g_StateLock;      /* serializes lookups against unregistering */
static BOOL g_ServerStarted;
static WLUI_SUBMIT_HANDLER g_SubmitHandler;
static WLUI_ACTION_HANDLER g_ActionHandler;

/* ---- what the rest of winlogon uses --------------------------------------- */

PWLUI_LAUNCH_STATE
WluiLaunchRegister(
    _In_ DWORD Pid,
    _In_ PSID Sid,
    _In_ WLUI_AUTH_ROLE Role,
    _In_opt_ const GUID *RequestedId,
    _Out_ GUID *LaunchId)
{
    PWLUI_LAUNCH_STATE State;

    ZeroMemory(LaunchId, sizeof(*LaunchId));
    if (!g_ServerStarted)
        return NULL;

    State = WluiStateCreate(sizeof(WLUI_COMMAND));
    if (State == NULL)
        return NULL;

    EnterCriticalSection(&g_StateLock);
    if (!WluiLaunchTableAdd(&g_LaunchTable, Pid, Sid, Role, State, RequestedId, LaunchId))
    {
        LeaveCriticalSection(&g_StateLock);
        WluiStateRelease(State);
        return NULL;
    }
    State->LaunchId = *LaunchId;
    LeaveCriticalSection(&g_StateLock);

    TRACE("WL: wlui launch registered pid %lu role %u\n", Pid, Role);
    return State;
}

VOID
WluiLaunchUnregister(
    _In_ PWLUI_LAUNCH_STATE State)
{
    EnterCriticalSection(&g_StateLock);
    WluiLaunchTableRemove(&g_LaunchTable, &State->LaunchId);
    WluiStateMarkDead(State);
    LeaveCriticalSection(&g_StateLock);
    WluiStateRelease(State);
}

/* Returns the command's sequence number, 0 if it was dropped. */
DWORD
WluiLaunchPostCommand(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ const struct _WLUI_COMMAND *Command)
{
    WLUI_COMMAND Copy = *Command;

    Copy.Sequence = (DWORD)InterlockedIncrement(&State->SequenceCounter);
    if (!WluiStatePost(State, &Copy, sizeof(Copy)))
    {
        ERR("WL: wlui command %u dropped (queue full or launch dead)\n", Copy.Type);
        return 0;
    }
    return Copy.Sequence;
}

BOOL
WluiLaunchWaitShown(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD Sequence,
    _In_ DWORD TimeoutMs)
{
    return Sequence != 0 && WluiStateWaitShown(State, Sequence, TimeoutMs);
}

BOOL
WluiLaunchWaitReady(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD TimeoutMs)
{
    return WluiStateWaitReady(State, TimeoutMs);
}

ULONGLONG
WluiLaunchLastHeartbeat(
    _In_ PWLUI_LAUNCH_STATE State)
{
    return WluiStateLastHeartbeat(State);
}

BOOL
WluiLaunchExited(
    _In_ PWLUI_LAUNCH_STATE State,
    _Out_opt_ DWORD *Reason)
{
    return WluiStateExited(State, Reason);
}

VOID
WluiSetSubmitHandler(
    _In_opt_ WLUI_SUBMIT_HANDLER Handler)
{
    g_SubmitHandler = Handler;
}

VOID
WluiSetActionHandler(
    _In_opt_ WLUI_ACTION_HANDLER Handler)
{
    g_ActionHandler = Handler;
}

BOOL
WluiServerStart(VOID)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    RPC_STATUS Status;

    InitializeCriticalSection(&g_StateLock);
    WluiLaunchTableInit(&g_LaunchTable);

    /* Only SYSTEM connects: the host and the recovery component run on winlogon's token. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYG:SYD:(A;;GA;;;SY)",
                                                              SDDL_REVISION_1, &Sd, NULL))
    {
        ERR("WL: wlui endpoint descriptor failed (error %lu)\n", GetLastError());
        return FALSE;
    }

    Status = RpcServerUseProtseqEpW(L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, L"wlui", Sd);
    LocalFree(Sd);
    if (Status != RPC_S_OK)
    {
        ERR("WL: RpcServerUseProtseqEpW(wlui) failed (Status %lx)\n", Status);
        return FALSE;
    }

    /* The interface would otherwise answer on \pipe\winreg too; the callback keeps it on ncalrpc. */
    Status = RpcServerRegisterIfEx(wlui_v1_0_s_ifspec, NULL, NULL, RPC_IF_ALLOW_LOCAL_ONLY,
                                   RPC_C_LISTEN_MAX_CALLS_DEFAULT, WluiServerIfCallback);
    if (Status != RPC_S_OK)
    {
        ERR("WL: RpcServerRegisterIfEx(wlui) failed (Status %lx)\n", Status);
        return FALSE;
    }

    g_ServerStarted = TRUE;
    return TRUE;
}

/* ---- server side of each call --------------------------------------------- */

static DWORD
WluiGetCaller(
    _In_ handle_t hBinding,
    _Out_ PDWORD Pid,
    _Out_writes_bytes_(SECURITY_MAX_SID_SIZE) PSID Sid)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    HANDLE Token;
    DWORD Length;
    BOOL Ok = FALSE;

    if (I_RpcBindingInqLocalClientPID(hBinding, Pid) != RPC_S_OK)
        return ERROR_ACCESS_DENIED;

    if (RpcImpersonateClient(hBinding) != RPC_S_OK)
        return ERROR_ACCESS_DENIED;

    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token))
    {
        Ok = GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length) &&
             CopySid(SECURITY_MAX_SID_SIZE, Sid, ((PTOKEN_USER)Buffer)->User.Sid);
        CloseHandle(Token);
    }
    RpcRevertToSelf();

    return Ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

/* Authorizes the call and returns the launch state with one reference held. */
static DWORD
WluiAuthorizeCall(
    _In_ handle_t hBinding,
    _In_ const GUID *LaunchId,
    _In_ WLUI_AUTH_OP Op,
    _Out_ PWLUI_LAUNCH_STATE *State,
    _Out_opt_ WLUI_AUTH_ROLE *Role)
{
    BYTE Sid[SECURITY_MAX_SID_SIZE];
    DWORD Pid, Error;
    WLUI_AUTH_RESULT Result;
    PVOID Context = NULL;

    *State = NULL;

    Error = WluiGetCaller(hBinding, &Pid, Sid);
    if (Error != ERROR_SUCCESS)
        return Error;

    EnterCriticalSection(&g_StateLock);
    Result = WluiAuthorize(&g_LaunchTable, Pid, (PSID)Sid, LaunchId, Op, Role, &Context);
    if (Result == WluiAuthOk && Context != NULL && !((PWLUI_LAUNCH_STATE)Context)->Dead)
    {
        *State = Context;
        WluiStateAddRef(*State);
    }
    else if (Result == WluiAuthOk)
    {
        Result = WluiAuthUnknownLaunch;
    }
    LeaveCriticalSection(&g_StateLock);

    if (Result != WluiAuthOk)
    {
        WARN("WL: wlui call %u refused for pid %lu: %ls\n", Op, Pid, WluiAuthResultName(Result));
        return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

DWORD
__stdcall
WluiReady(
    handle_t hBinding,
    GUID LaunchId)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpReady, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    WluiStateSetReady(State);
    WluiStateRelease(State);
    return ERROR_SUCCESS;
}

DWORD
__stdcall
WluiHeartbeat(
    handle_t hBinding,
    GUID LaunchId)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpHeartbeat, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    WluiStateTouch(State);
    WluiStateRelease(State);
    return ERROR_SUCCESS;
}

DWORD
__stdcall
WluiWaitForCommand(
    handle_t hBinding,
    GUID LaunchId,
    WLUI_COMMAND *Command)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;
    BOOL Got;

    ZeroMemory(Command, sizeof(*Command));
    Command->Type = WluiCmdNone;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpWaitForCommand, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* One wait per launch pins one server thread at most (ERROR_BUSY to a second). */
    Error = WluiStateWait(State, WLUI_COMMAND_WAIT_MS, Command, sizeof(*Command), &Got);
    if (Error == ERROR_SUCCESS && !Got)
        Command->Type = WluiCmdNone;

    WluiStateRelease(State);
    return Error;
}

DWORD
__stdcall
WluiSubmitSerialization(
    handle_t hBinding,
    GUID LaunchId,
    DWORD Scenario,
    GUID ProviderClsid,
    DWORD AuthenticationPackage,
    DWORD cbSerialization,
    const byte *Serialization)
{
    PWLUI_LAUNCH_STATE State;
    WLUI_AUTH_ROLE Role;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpSubmitSerialization, &State, &Role);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* No logon flow is attached until the host-mode logon path registers one. */
    if (g_SubmitHandler == NULL)
        Error = ERROR_NOT_READY;
    else
        Error = g_SubmitHandler(State, Role, Scenario, &ProviderClsid, AuthenticationPackage,
                                Serialization, cbSerialization);

    WluiStateRelease(State);
    return Error;
}

DWORD
__stdcall
WluiRequestAction(
    handle_t hBinding,
    GUID LaunchId,
    WLUI_ACTION Action)
{
    PWLUI_LAUNCH_STATE State;
    WLUI_AUTH_ROLE Role;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpRequestAction, &State, &Role);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (g_ActionHandler == NULL)
        Error = ERROR_NOT_READY;
    else
        Error = g_ActionHandler(State, Role, Action);

    WluiStateRelease(State);
    return Error;
}

DWORD
__stdcall
WluiReportProviderFault(
    handle_t hBinding,
    GUID LaunchId,
    GUID ProviderClsid,
    DWORD Reason)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpReportProviderFault, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    WluiStateRecordFault(State, &ProviderClsid, Reason);
    ERR("WL: credential provider {%08lX-...} discarded by the host (reason %lu)\n",
        ProviderClsid.Data1, Reason);

    WluiStateRelease(State);
    return ERROR_SUCCESS;
}

DWORD
__stdcall
WluiExiting(
    handle_t hBinding,
    GUID LaunchId,
    WLUI_EXIT_REASON Reason)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpExiting, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    WluiStateSetExiting(State, (DWORD)Reason);
    TRACE("WL: wlui launch exiting (reason %u)\n", Reason);

    WluiStateRelease(State);
    return ERROR_SUCCESS;
}

/* Function 7 */
DWORD
__stdcall
WluiCommandShown(
    handle_t hBinding,
    GUID LaunchId,
    DWORD Sequence)
{
    PWLUI_LAUNCH_STATE State;
    DWORD Error;

    Error = WluiAuthorizeCall(hBinding, &LaunchId, WluiOpCommandShown, &State, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Only a sequence that was actually handed out counts (the caller is untrusted input). */
    Error = WluiStateSetShown(State, Sequence) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
    WluiStateRelease(State);
    return Error;
}
