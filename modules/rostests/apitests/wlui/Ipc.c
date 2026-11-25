/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the wlui client: the server check (P1) and the wire
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <sddl.h>
#include <strsafe.h>
#include <wluiclient.h>
#include <wluiauth.h>

/* The server stubs (wlui_s.c) call these; their header repeats the client types, so take the ifspec alone. */
extern RPC_IF_HANDLE wlui_v1_0_s_ifspec;

/* ---- the test server: records what arrives ------------------------------- */

static LONG g_Calls;
static GUID g_LastLaunchId;
static DWORD g_LastScenario, g_LastCb, g_LastAction, g_LastFaultReason, g_LastExitReason;
static GUID g_LastClsid;
static BYTE g_LastBytes[32];

DWORD __stdcall WluiReady(handle_t h, GUID LaunchId)
{
    InterlockedIncrement(&g_Calls); g_LastLaunchId = LaunchId; return ERROR_SUCCESS;
}

DWORD __stdcall WluiHeartbeat(handle_t h, GUID LaunchId)
{
    InterlockedIncrement(&g_Calls); g_LastLaunchId = LaunchId; return ERROR_SUCCESS;
}

DWORD __stdcall WluiWaitForCommand(handle_t h, GUID LaunchId, WLUI_COMMAND *Command)
{
    InterlockedIncrement(&g_Calls);
    g_LastLaunchId = LaunchId;
    ZeroMemory(Command, sizeof(*Command));
    Command->Type = WluiCmdDisplayStatus;
    Command->Flags = WLUI_FLAG_SPINNER;
    Command->Sequence = 7;
    StringCchCopyW(Command->Text, ARRAYSIZE(Command->Text), L"Welcome");
    return ERROR_SUCCESS;
}

DWORD __stdcall WluiSubmitSerialization(handle_t h, GUID LaunchId, DWORD Scenario, GUID ProviderClsid,
                                        DWORD AuthenticationPackage, DWORD cbSerialization,
                                        const byte *Serialization)
{
    InterlockedIncrement(&g_Calls);
    g_LastLaunchId = LaunchId;
    g_LastScenario = Scenario;
    g_LastClsid = ProviderClsid;
    g_LastCb = cbSerialization;
    CopyMemory(g_LastBytes, Serialization, min(cbSerialization, sizeof(g_LastBytes)));
    return AuthenticationPackage == 0 ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
}

DWORD __stdcall WluiRequestAction(handle_t h, GUID LaunchId, WLUI_ACTION Action)
{
    InterlockedIncrement(&g_Calls); g_LastLaunchId = LaunchId; g_LastAction = Action; return ERROR_SUCCESS;
}

DWORD __stdcall WluiReportProviderFault(handle_t h, GUID LaunchId, GUID ProviderClsid, DWORD Reason)
{
    InterlockedIncrement(&g_Calls); g_LastLaunchId = LaunchId; g_LastClsid = ProviderClsid;
    g_LastFaultReason = Reason; return ERROR_SUCCESS;
}

DWORD __stdcall WluiExiting(handle_t h, GUID LaunchId, WLUI_EXIT_REASON Reason)
{
    InterlockedIncrement(&g_Calls); g_LastLaunchId = LaunchId; g_LastExitReason = Reason; return ERROR_SUCCESS;
}

/* ---- helpers --------------------------------------------------------------- */

static PSID
GetOwnSid(void)
{
    static BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    HANDLE Token;
    DWORD Length;
    PSID Sid = NULL;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length))
            Sid = ((PTOKEN_USER)Buffer)->User.Sid;
        CloseHandle(Token);
    }
    return Sid;
}

static BOOL
StartTestServer(
    _In_ PCWSTR Endpoint,
    _In_ PSID OwnSid)
{
    WCHAR Sddl[128];
    PWSTR SidString = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    RPC_STATUS Status;

    if (!ConvertSidToStringSidW(OwnSid, &SidString))
        return FALSE;
    StringCchPrintfW(Sddl, ARRAYSIZE(Sddl), L"D:(A;;GA;;;%s)", SidString);
    LocalFree(SidString);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, NULL))
        return FALSE;

    Status = RpcServerUseProtseqEpW(L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)Endpoint, Sd);
    ok(Status == RPC_S_OK, "UseProtseqEp %lu\n", Status);
    if (Status == RPC_S_OK)
    {
        /* A second, named-pipe listener: the interface must still refuse calls that arrive on it. */
        WCHAR Pipe[96];
        StringCchPrintfW(Pipe, ARRAYSIZE(Pipe), L"\\pipe\\%s", Endpoint);
        Status = RpcServerUseProtseqEpW(L"ncacn_np", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)Pipe, Sd);
        ok(Status == RPC_S_OK, "UseProtseqEp(ncacn_np) %lu\n", Status);
    }
    LocalFree(Sd);
    if (Status != RPC_S_OK)
        return FALSE;
    /* Registered exactly as winlogon does: local only, and the callback keeps it on ncalrpc. */
    Status = RpcServerRegisterIfEx(wlui_v1_0_s_ifspec, NULL, NULL, RPC_IF_ALLOW_LOCAL_ONLY,
                                   RPC_C_LISTEN_MAX_CALLS_DEFAULT, WluiServerIfCallback);
    ok(Status == RPC_S_OK, "RegisterIfEx %lu\n", Status);
    if (Status != RPC_S_OK)
        return FALSE;
    Status = RpcServerListen(1, 4, TRUE);
    ok(Status == RPC_S_OK || Status == RPC_S_ALREADY_LISTENING, "Listen %lu\n", Status);
    return Status == RPC_S_OK || Status == RPC_S_ALREADY_LISTENING;
}

static void
StopTestServer(void)
{
    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(wlui_v1_0_s_ifspec, NULL, TRUE);
}

/* ---- tests ------------------------------------------------------------------ */

static void
Test_NoServer(void)
{
    WLUI_CONNECTION C;
    GUID Id = { 1, 2, 3, { 4, 5, 6, 7, 8, 9, 10, 11 } };
    BYTE System[SECURITY_MAX_SID_SIZE];
    DWORD Error;

    ok(WluiGetLocalSystemSid(System), "system sid\n");
    ok(WluiConnect(L"wlui-apitest-nobody-here", System, &Id, &C) == RPC_S_OK, "bind without a server\n");
    Error = WluiClientHeartbeat(&C);
    ok(Error == RPC_S_SERVER_UNAVAILABLE, "no server: %lu\n", Error);
    WluiDisconnect(&C);
    ok(C.Binding == NULL, "binding not cleared\n");

    ok(WluiConnect(NULL, NULL, &Id, &C) == RPC_S_INVALID_ARG, "NULL sid accepted\n");
    ok(WluiConnect(NULL, System, NULL, &C) == RPC_S_INVALID_ARG, "NULL launch id accepted\n");
}

static void
Test_ServerCheckAndWire(void)
{
    WCHAR Endpoint[64];
    WLUI_CONNECTION C;
    WLUI_COMMAND Cmd;
    GUID Id = { 0xA1B2C3D4, 0x5, 0x6, { 7, 8, 9, 10, 11, 12, 13, 14 } };
    GUID Clsid = { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } };
    BYTE System[SECURITY_MAX_SID_SIZE];
    BYTE Blob[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    PSID Own = GetOwnSid();
    BOOL RunningAsSystem;
    DWORD Error;

    ok(Own != NULL, "own sid\n");
    ok(WluiGetLocalSystemSid(System), "system sid\n");
    if (Own == NULL)
        return;
    RunningAsSystem = EqualSid(Own, System);

    StringCchPrintfW(Endpoint, ARRAYSIZE(Endpoint), L"wlui-apitest-%lu", GetCurrentProcessId());
    if (!StartTestServer(Endpoint, Own))
        return;

    /* P1: the client names SYSTEM. A server owned by anyone else is refused before any
     * call reaches it; running as SYSTEM (VM leg) the same connection succeeds. */
    g_Calls = 0;
    ok(WluiConnect(Endpoint, System, &Id, &C) == RPC_S_OK, "connect (system expected)\n");
    Error = WluiClientHeartbeat(&C);
    if (RunningAsSystem)
    {
        ok(Error == ERROR_SUCCESS, "as SYSTEM: %lu\n", Error);
        ok(g_Calls == 1, "as SYSTEM: calls %ld\n", g_Calls);
    }
    else
    {
        ok(Error == RPC_S_ACCESS_DENIED, "impostor server: %lu (expected 5)\n", Error);
        ok(g_Calls == 0, "impostor server received %ld call(s)\n", g_Calls);
    }
    WluiDisconnect(&C);

    /* R1: the same interface reached over ncacn_np is refused by the interface callback
     * before any handler runs, even for a caller that could pass the authorizer. */
    {
        RPC_WSTR StringBinding = NULL;
        RPC_BINDING_HANDLE Np = NULL;
        WCHAR Pipe[96];
        DWORD NpError = 0;

        StringCchPrintfW(Pipe, ARRAYSIZE(Pipe), L"\\pipe\\%s", Endpoint);
        ok(RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncacn_np", (RPC_WSTR)L".", (RPC_WSTR)Pipe, NULL,
                                    &StringBinding) == RPC_S_OK, "np compose\n");
        ok(RpcBindingFromStringBindingW(StringBinding, &Np) == RPC_S_OK, "np binding\n");
        RpcStringFreeW(&StringBinding);
        g_Calls = 0;
        RpcTryExcept
        {
            NpError = WluiHeartbeat(Np, Id);
        }
        RpcExcept(EXCEPTION_EXECUTE_HANDLER)
        {
            NpError = RpcExceptionCode();
        }
        RpcEndExcept;
        ok(NpError == RPC_S_ACCESS_DENIED, "over ncacn_np: %lu (expected 5)\n", NpError);
        ok(g_Calls == 0, "ncacn_np call reached a handler (%ld)\n", g_Calls);
        RpcBindingFree(&Np);
    }

    /* The wire, against the server we own: name our own SID and every call goes through. */
    g_Calls = 0;
    ok(WluiConnect(Endpoint, Own, &Id, &C) == RPC_S_OK, "connect (own sid)\n");

    ok(WluiClientReady(&C) == ERROR_SUCCESS, "Ready\n");
    ok(IsEqualGUID(&g_LastLaunchId, &Id), "launch id did not arrive\n");
    ok(WluiClientHeartbeat(&C) == ERROR_SUCCESS, "Heartbeat\n");

    ok(WluiClientWaitForCommand(&C, &Cmd) == ERROR_SUCCESS, "WaitForCommand\n");
    ok(Cmd.Type == WluiCmdDisplayStatus, "cmd type %u\n", Cmd.Type);
    ok(Cmd.Flags == WLUI_FLAG_SPINNER, "cmd flags %lx\n", Cmd.Flags);
    ok(Cmd.Sequence == 7, "cmd sequence %lu\n", Cmd.Sequence);
    ok(wcscmp(Cmd.Text, L"Welcome") == 0, "cmd text %ls\n", Cmd.Text);

    ok(WluiClientSubmitSerialization(&C, 2, &Clsid, 10, Blob, sizeof(Blob)) == ERROR_SUCCESS, "Submit\n");
    ok(g_LastScenario == 2 && g_LastCb == 16 && IsEqualGUID(&g_LastClsid, &Clsid), "submit args\n");
    ok(memcmp(g_LastBytes, Blob, sizeof(Blob)) == 0, "submit bytes\n");
    ok(WluiClientSubmitSerialization(&C, 2, &Clsid, 0, Blob, sizeof(Blob)) == ERROR_INVALID_PARAMETER,
       "server status not propagated\n");
    ok(WluiClientSubmitSerialization(&C, 2, &Clsid, 10, Blob, 0) == ERROR_SUCCESS, "empty blob\n");
    ok(g_LastCb == 0, "empty blob cb %lu\n", g_LastCb);
    ok(WluiClientSubmitSerialization(&C, 2, &Clsid, 10, NULL, 4) == ERROR_INVALID_PARAMETER, "NULL blob\n");
    ok(WluiClientSubmitSerialization(&C, 2, &Clsid, 10, Blob, WLUI_MAX_SERIALIZATION + 1) == ERROR_INVALID_PARAMETER,
       "oversized blob\n");
    ok(WluiClientSubmitSerialization(&C, 2, NULL, 10, Blob, 4) == ERROR_INVALID_PARAMETER, "NULL clsid\n");

    ok(WluiClientRequestAction(&C, WluiActionRestart) == ERROR_SUCCESS, "RequestAction\n");
    ok(g_LastAction == WluiActionRestart, "action %lu\n", g_LastAction);
    ok(WluiClientReportProviderFault(&C, &Clsid, 3) == ERROR_SUCCESS, "ReportProviderFault\n");
    ok(g_LastFaultReason == 3, "fault reason %lu\n", g_LastFaultReason);
    ok(WluiClientExiting(&C, WluiExitDismissed) == ERROR_SUCCESS, "Exiting\n");
    ok(g_LastExitReason == WluiExitDismissed, "exit reason %lu\n", g_LastExitReason);
    /* Ready, Heartbeat, WaitForCommand, three submits that pass the client checks
     * (the package-0 one is refused by the server, after it counted), action, fault, exiting. */
    ok(g_Calls == 9, "calls %ld\n", g_Calls);

    WluiDisconnect(&C);
    StopTestServer();
}

START_TEST(Ipc)
{
    Test_NoServer();
    Test_ServerCheckAndWire();
}
