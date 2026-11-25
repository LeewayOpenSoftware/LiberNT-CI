/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Drives logonrecovery_test.exe through a private wlui server
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <sddl.h>
#include <strsafe.h>
#include <wluiclient.h>
#include <wluiauth.h>
#include <ntsecapi.h>
#include <credser.h>

extern RPC_IF_HANDLE wlui_v1_0_s_ifspec;

static const GUID CLSID_PasswordProvider =
    { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } };

/* ---- the test server ------------------------------------------------------ */

static PWLUI_LAUNCH_STATE g_State;
static HANDLE g_ReadyEvent, g_ExitEvent, g_SubmitEvent;
static LONG g_Heartbeats;
static DWORD g_ExitReason = 0xFFFFFFFF;
static GUID g_ExpectedId;
static LONG g_WrongId;
static DWORD g_Action;

/* What the last submission carried, checked on the test thread. */
static DWORD g_SubmitScenario, g_SubmitPackage, g_SubmitSize;
static GUID g_SubmitClsid;
static BYTE g_SubmitBlob[CREDSER_MAX_BLOB_SIZE];

static void CheckId(const GUID *Id) { if (!IsEqualGUID(Id, &g_ExpectedId)) InterlockedIncrement(&g_WrongId); }

DWORD __stdcall WluiReady(handle_t h, GUID LaunchId)
{
    CheckId(&LaunchId); SetEvent(g_ReadyEvent); return ERROR_SUCCESS;
}

DWORD __stdcall WluiHeartbeat(handle_t h, GUID LaunchId)
{
    CheckId(&LaunchId); InterlockedIncrement(&g_Heartbeats); return ERROR_SUCCESS;
}

DWORD __stdcall WluiWaitForCommand(handle_t h, GUID LaunchId, WLUI_COMMAND *Command)
{
    BOOL Got;
    DWORD Error;

    CheckId(&LaunchId);
    Error = WluiStateWait(g_State, 2000, Command, sizeof(*Command), &Got);
    if (Error == ERROR_SUCCESS && !Got)
        Command->Type = WluiCmdNone;
    return Error;
}

DWORD __stdcall WluiSubmitSerialization(handle_t h, GUID LaunchId, DWORD Scenario, GUID ProviderClsid,
                                        DWORD AuthenticationPackage, DWORD cbSerialization,
                                        const byte *Serialization)
{
    CheckId(&LaunchId);
    g_SubmitScenario = Scenario;
    g_SubmitClsid = ProviderClsid;
    g_SubmitPackage = AuthenticationPackage;
    g_SubmitSize = cbSerialization <= sizeof(g_SubmitBlob) ? cbSerialization : 0;
    if (g_SubmitSize)
        CopyMemory(g_SubmitBlob, Serialization, g_SubmitSize);
    SetEvent(g_SubmitEvent);
    return ERROR_SUCCESS;
}

DWORD __stdcall WluiRequestAction(handle_t h, GUID LaunchId, WLUI_ACTION Action)
{
    CheckId(&LaunchId); g_Action = Action; return ERROR_SUCCESS;
}

DWORD __stdcall WluiReportProviderFault(handle_t h, GUID LaunchId, GUID ProviderClsid, DWORD Reason)
{
    return ERROR_NOT_READY;     /* the recovery dialog has no providers to report */
}

DWORD __stdcall WluiExiting(handle_t h, GUID LaunchId, WLUI_EXIT_REASON Reason)
{
    CheckId(&LaunchId); g_ExitReason = Reason; SetEvent(g_ExitEvent); return ERROR_SUCCESS;
}

/* ---- helpers --------------------------------------------------------------- */

static PSID
GetOwnSid(void)
{
    static BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    HANDLE Token;
    DWORD Length;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return NULL;
    if (!GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length))
    {
        CloseHandle(Token);
        return NULL;
    }
    CloseHandle(Token);
    return ((PTOKEN_USER)Buffer)->User.Sid;
}

static BOOL
StartServer(PCWSTR Endpoint, PSID Own)
{
    WCHAR Sddl[128];
    PWSTR SidString = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    RPC_STATUS Status;

    if (!ConvertSidToStringSidW(Own, &SidString))
        return FALSE;
    StringCchPrintfW(Sddl, ARRAYSIZE(Sddl), L"D:(A;;GA;;;%s)", SidString);
    LocalFree(SidString);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, NULL))
        return FALSE;
    Status = RpcServerUseProtseqEpW(L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)Endpoint, Sd);
    LocalFree(Sd);
    if (Status != RPC_S_OK)
        return FALSE;
    if (RpcServerRegisterIfEx(wlui_v1_0_s_ifspec, NULL, NULL, RPC_IF_ALLOW_LOCAL_ONLY,
                              RPC_C_LISTEN_MAX_CALLS_DEFAULT, WluiServerIfCallback) != RPC_S_OK)
        return FALSE;
    Status = RpcServerListen(1, 4, TRUE);
    return Status == RPC_S_OK || Status == RPC_S_ALREADY_LISTENING;
}

static BOOL
ExePath(PWSTR Path, SIZE_T Count)
{
    PWSTR Slash;
    if (!GetModuleFileNameW(NULL, Path, (DWORD)Count))
        return FALSE;
    Slash = wcsrchr(Path, L'\\');
    if (!Slash)
        return FALSE;
    Slash[1] = UNICODE_NULL;
    return SUCCEEDED(StringCchCatW(Path, Count, L"logonrecovery_test.exe"));
}

static BOOL
VisibleWindowStation(void)
{
    USEROBJECTFLAGS Flags;
    DWORD Length;
    HWINSTA Station = GetProcessWindowStation();
    return Station && GetUserObjectInformationW(Station, UOI_FLAGS, &Flags, sizeof(Flags), &Length) &&
           (Flags.dwFlags & WSF_VISIBLE);
}

static HANDLE
Launch(PCWSTR Exe, PCWSTR Endpoint, PSID Own, PCWSTR Trace, DWORD Scenario, PCWSTR Extra)
{
    WCHAR Cmd[1024], Id[64];
    PWSTR SidString = NULL;
    RPC_WSTR IdString = NULL;
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    BOOL Ok;

    UuidToStringW(&g_ExpectedId, &IdString);
    StringCchPrintfW(Id, ARRAYSIZE(Id), L"{%s}", (PCWSTR)IdString);
    RpcStringFreeW(&IdString);
    ConvertSidToStringSidW(Own, &SidString);
    StringCchPrintfW(Cmd, ARRAYSIZE(Cmd),
                     L"\"%s\" /launchid:%s /scenario:%lu /endpoint:%s /expect-server-sid:%s /trace:%s %s",
                     Exe, Id, Scenario, Endpoint, SidString, Trace, Extra ? Extra : L"");
    LocalFree(SidString);

    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Ok = CreateProcessW(Exe, Cmd, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi);
    if (!Ok)
        return NULL;
    CloseHandle(Pi.hThread);
    return Pi.hProcess;
}

static DWORD
WaitExit(HANDLE Process, DWORD TimeoutMs)
{
    DWORD Code = 0xFFFFFFFF;
    if (WaitForSingleObject(Process, TimeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(Process, &Code);
    else
        TerminateProcess(Process, 0xDEAD);
    CloseHandle(Process);
    return Code;
}

static BOOL
TraceHas(PCWSTR Trace, PCSTR Needle)
{
    HANDLE File = CreateFileW(Trace, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHAR Buffer[8192];
    DWORD Read = 0;
    BOOL Found = FALSE;

    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    if (ReadFile(File, Buffer, sizeof(Buffer) - 1, &Read, NULL))
    {
        Buffer[Read] = 0;
        Found = strstr(Buffer, Needle) != NULL;
    }
    CloseHandle(File);
    return Found;
}

static void
PostCmd(WLUI_COMMAND_TYPE Type, DWORD Scenario, DWORD Flags, LONG Status, PCWSTR Text, PCWSTR User)
{
    WLUI_COMMAND Cmd;
    ZeroMemory(&Cmd, sizeof(Cmd));
    Cmd.Type = Type;
    Cmd.Scenario = Scenario;
    Cmd.Flags = Flags;
    Cmd.Status = Status;
    if (Text) StringCchCopyW(Cmd.Text, ARRAYSIZE(Cmd.Text), Text);
    if (User) StringCchCopyW(Cmd.User, ARRAYSIZE(Cmd.User), User);
    ok(WluiStatePost(g_State, &Cmd, sizeof(Cmd)), "post %u\n", Type);
}

/* One launch state per run, as winlogon keeps one per launch: the previous twin's abandoned
 * WaitForCommand can sit in the old state for up to 2 s and would take the next command for a
 * connection that is gone. The old state is marked dead (the stale wait returns
 * ERROR_OPERATION_ABORTED) and left to process exit, since that handler may still be inside it. */
static void
ResetRun(void)
{
    ResetEvent(g_ReadyEvent);
    ResetEvent(g_ExitEvent);
    ResetEvent(g_SubmitEvent);
    g_Heartbeats = 0;
    g_ExitReason = 0xFFFFFFFF;
    g_Action = 0;
    g_SubmitSize = 0;
    SecureZeroMemory(g_SubmitBlob, sizeof(g_SubmitBlob));
    if (g_State)
        WluiStateMarkDead(g_State);
    g_State = WluiStateCreate(sizeof(WLUI_COMMAND));
}

/* The blob winlogon would receive: captured, unpacked and mapped exactly as hostlogon.c does. */
static void
CheckLogonBlob(PCWSTR ExpectDomain, PCWSTR ExpectUser, PCWSTR ExpectPassword, BOOL Unlock)
{
    PVOID Copy = NULL;
    CREDSER_LOGON Logon;
    PMSV1_0_INTERACTIVE_LOGON Msv = NULL;
    ULONG MsvSize = 0;
    NTSTATUS Status;

    ok(g_SubmitSize != 0, "no serialization received\n");
    if (g_SubmitSize == 0)
        return;
    Status = CredSerCapture(g_SubmitBlob, g_SubmitSize, &Copy);
    ok(NT_SUCCESS(Status), "CredSerCapture 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    Status = CredSerUnpackLogon(Copy, g_SubmitSize, &Logon);
    ok(NT_SUCCESS(Status), "CredSerUnpackLogon 0x%08lx (the blob does not validate)\n", Status);
    if (NT_SUCCESS(Status))
    {
        ok(Logon.MessageType == (Unlock ? KerbWorkstationUnlockLogon : KerbInteractiveLogon),
           "message type %u\n", Logon.MessageType);
        ok(Logon.UserName.Length == wcslen(ExpectUser) * sizeof(WCHAR) &&
           memcmp(Logon.UserName.Buffer, ExpectUser, Logon.UserName.Length) == 0, "user name differs\n");
        ok(Logon.Password.Length == wcslen(ExpectPassword) * sizeof(WCHAR) &&
           memcmp(Logon.Password.Buffer, ExpectPassword, Logon.Password.Length) == 0, "password differs\n");
        if (ExpectDomain)
            ok(Logon.LogonDomainName.Length == wcslen(ExpectDomain) * sizeof(WCHAR) &&
               memcmp(Logon.LogonDomainName.Buffer, ExpectDomain, Logon.LogonDomainName.Length) == 0, "domain differs\n");
        else
            ok(Logon.LogonDomainName.Length == 0, "domain expected empty, got %u bytes\n", Logon.LogonDomainName.Length);
        Status = CredSerBuildMsv1_0Logon(&Logon, &Msv, &MsvSize);
        ok(NT_SUCCESS(Status), "CredSerBuildMsv1_0Logon 0x%08lx (the blob does not map)\n", Status);
        if (Msv)
            CredSerFree(Msv, MsvSize);
    }
    CredSerFree(Copy, g_SubmitSize);
}

/* ---- the test --------------------------------------------------------------- */

START_TEST(Recovery)
{
    WCHAR Exe[MAX_PATH], Endpoint[64], Trace[MAX_PATH], Temp[MAX_PATH];
    PSID Own = GetOwnSid();
    HANDLE Process;
    DWORD Code;

    if (!VisibleWindowStation())
    {
        skip("no visible window station; the dialog cannot be created here\n");
        return;
    }
    ok(ExePath(Exe, ARRAYSIZE(Exe)), "exe path\n");
    if (GetFileAttributesW(Exe) == INVALID_FILE_ATTRIBUTES)
    {
        skip("%ls not found\n", Exe);
        return;
    }
    ok(Own != NULL, "own sid\n");
    UuidCreate(&g_ExpectedId);
    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(Trace, ARRAYSIZE(Trace), L"%slrc-%lu.log", Temp, GetCurrentProcessId());
    StringCchPrintfW(Endpoint, ARRAYSIZE(Endpoint), L"wlui-lrc-%lu", GetCurrentProcessId());

    g_State = WluiStateCreate(sizeof(WLUI_COMMAND));
    g_ReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_SubmitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Bad arguments: no launch id. */
    {
        WCHAR Cmd[256];
        STARTUPINFOW Si;
        PROCESS_INFORMATION Pi;
        ZeroMemory(&Si, sizeof(Si));
        Si.cb = sizeof(Si);
        StringCchPrintfW(Cmd, ARRAYSIZE(Cmd), L"\"%s\" /scenario:1", Exe);
        ok(CreateProcessW(Exe, Cmd, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi), "launch (bad args)\n");
        CloseHandle(Pi.hThread);
        Code = WaitExit(Pi.hProcess, 5000);
        ok(Code == 1, "bad args exit %lu\n", Code);
    }

    /* No server behind the endpoint: give up with the IPC code, never hang. */
    DeleteFileW(Trace);
    Process = Launch(Exe, L"wlui-lrc-nobody", Own, Trace, 1, NULL);
    ok(Process != NULL, "launch (no server)\n");
    Code = WaitExit(Process, 10000);
    ok(Code == 2, "no server exit %lu\n", Code);

    if (!StartServer(Endpoint, Own))
    {
        skip("cannot start the test server\n");
        return;
    }

    /* P1: a server that is not the expected principal is refused by the client itself. */
    {
        PSID Nobody = NULL;
        if (ConvertStringSidToSidW(L"S-1-5-7", &Nobody))     /* ANONYMOUS LOGON: not us */
        {
            DeleteFileW(Trace);
            Process = Launch(Exe, Endpoint, Nobody, Trace, 1, NULL);
            ok(Process != NULL, "launch (wrong server sid)\n");
            Code = WaitExit(Process, 10000);
            ok(Code == 2, "wrong server principal: exit %lu (expected 2, the client must not talk)\n", Code);
            ok(WaitForSingleObject(g_ReadyEvent, 0) != WAIT_OBJECT_0, "the client sent Ready to a server it should not trust\n");
            LocalFree(Nobody);
        }
    }

    /* Run 1: Ready after the dialog is shown, heartbeats, a logon page, a submission that
     * validates and maps, zeroed buffers, a result, then Dismiss with exit 0. */
    ResetRun();
    DeleteFileW(Trace);
    Process = Launch(Exe, Endpoint, Own, Trace, 1, L"/autosubmit:ORSI\\legtest;Pa55w.rd");
    ok(Process != NULL, "launch 1\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready\n");
    Sleep(4500);
    ok(TraceHas(Trace, "ready"), "ready not traced\n");    /* written after the Ready call returns */
    ok(g_Heartbeats >= 1, "heartbeats %ld\n", g_Heartbeats);
    ok(g_SubmitSize == 0, "a submission before RequestCredentials\n");
    PostCmd(WluiCmdRequestCredentials, 1, 0, 0, NULL, NULL);
    ok(WaitForSingleObject(g_SubmitEvent, 5000) == WAIT_OBJECT_0, "no submission after the page\n");
    ok(g_SubmitScenario == 1, "submitted scenario %lu\n", g_SubmitScenario);
    ok(IsEqualGUID(&g_SubmitClsid, &CLSID_PasswordProvider), "submitted CLSID is not the PasswordProvider\n");
    ok(g_SubmitPackage == 0, "submitted package %lu (expected 0: winlogon's MSV1_0)\n", g_SubmitPackage);
    CheckLogonBlob(L"ORSI", L"legtest", L"Pa55w.rd", FALSE);
    Sleep(300);
    ok(TraceHas(Trace, "submitted ok"), "submission not traced as sent\n");
    ok(TraceHas(Trace, "zeroed 1"), "the serialization buffer was not wiped after the send\n");
    PostCmd(WluiCmdReportResult, 1, 0, (LONG)0xC000006DL, NULL, NULL);
    Sleep(500);
    ok(TraceHas(Trace, "result 0xC000006D"), "result not traced\n");
    PostCmd(WluiCmdDisplayStatus, 1, WLUI_FLAG_SPINNER, 0, L"Welcome", NULL);
    PostCmd(WluiCmdDismiss, 0, 0, 0, NULL, NULL);
    ok(WaitForSingleObject(g_ExitEvent, 5000) == WAIT_OBJECT_0, "no Exiting after Dismiss\n");
    ok(g_ExitReason == WluiExitDismissed, "exit reason %lu\n", g_ExitReason);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "run 1 exit %lu\n", Code);

    /* Run 2: the unlock scenario maps to the unlock message type; a bare name is a local account. */
    ResetRun();
    DeleteFileW(Trace);
    Process = Launch(Exe, Endpoint, Own, Trace, 2, L"/autosubmit:legtest;Pa55w.rd");
    ok(Process != NULL, "launch 2\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (2)\n");
    PostCmd(WluiCmdRequestCredentials, 2, WLUI_FLAG_NO_POWER, 0, NULL, NULL);
    ok(WaitForSingleObject(g_SubmitEvent, 5000) == WAIT_OBJECT_0, "no submission (2)\n");
    ok(g_SubmitScenario == 2, "submitted scenario %lu (2)\n", g_SubmitScenario);
    CheckLogonBlob(NULL, L"legtest", L"Pa55w.rd", TRUE);
    ok(TraceHas(Trace, "page unlock"), "unlock page not traced\n");
    PostCmd(WluiCmdReadyForDesktopSwitch, 0, 0, 0, NULL, NULL);
    ok(WaitForSingleObject(g_ExitEvent, 5000) == WAIT_OBJECT_0, "no Exiting after the switch\n");
    ok(g_ExitReason == WluiExitAfterDesktopSwitch, "exit reason %lu (2)\n", g_ExitReason);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "run 2 exit %lu\n", Code);

    /* Run 3: a forced change page packs the change request (old/new), and a dismissed
     * dialog without any page leaves cleanly. */
    ResetRun();
    DeleteFileW(Trace);
    Process = Launch(Exe, Endpoint, Own, Trace, 3, L"/autosubmit:legtest;N3wPa55");
    ok(Process != NULL, "launch 3\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (3)\n");
    PostCmd(WluiCmdRequestCredentials, 3, WLUI_FLAG_FORCED_CHANGE, 0, NULL, NULL);
    ok(WaitForSingleObject(g_SubmitEvent, 5000) == WAIT_OBJECT_0, "no submission (3)\n");
    ok(g_SubmitScenario == 3, "submitted scenario %lu (3)\n", g_SubmitScenario);
    ok(TraceHas(Trace, "page change"), "change page not traced\n");
    {
        PVOID Copy = NULL;
        CREDSER_CHANGE_PASSWORD Change;
        NTSTATUS Status = CredSerCapture(g_SubmitBlob, g_SubmitSize, &Copy);
        ok(NT_SUCCESS(Status), "capture (3) 0x%08lx\n", Status);
        if (NT_SUCCESS(Status))
        {
            Status = CredSerUnpackChangePassword(Copy, g_SubmitSize, &Change);
            ok(NT_SUCCESS(Status), "CredSerUnpackChangePassword 0x%08lx\n", Status);
            if (NT_SUCCESS(Status))
                ok(Change.NewPassword.Length == 7 * sizeof(WCHAR) &&
                   memcmp(Change.NewPassword.Buffer, L"N3wPa55", Change.NewPassword.Length) == 0, "new password differs\n");
            CredSerFree(Copy, g_SubmitSize);
        }
    }
    PostCmd(WluiCmdDismiss, 0, 0, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "run 3 exit %lu\n", Code);

    ok(g_WrongId == 0, "%ld calls carried a wrong launch id\n", g_WrongId);
    ok(g_Action == 0, "a power action (%lu) was requested without a button press\n", g_Action);

    DeleteFileW(Trace);
    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(wlui_v1_0_s_ifspec, NULL, TRUE);
}
