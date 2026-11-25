/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launching and supervising the logon UI process on the Winlogon
 *              desktop, driven by the recovery levels (PHASE3-PLAN §8.9).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#include <rpc.h>
#include <wlimgcheck.h>
#include <wlui_s.h>

#define WLHOST_HEARTBEAT_MS   2000
#define WLHOST_HANG_MS        (3 * WLHOST_HEARTBEAT_MS)   /* three missed beats */
#define WLHOST_STARTUP_MS     10000  /* before Ready a host may still be loading providers */
#define WLHOST_DISMISS_MS     3000
#define WLHOST_SUPERVISOR_MS  5000   /* past this the entry is leaked, never freed under the supervisor */
#define WLHOST_LOGONUI_EXE    L"logonuihost.exe"
#define WLHOST_RECOVERY_EXE   L"logonrecovery.exe"

/* Exit codes the host uses (logonuihost.h); only the ones the supervisor interprets. */
#define WLHOST_EXIT_OK            0
#define WLHOST_EXIT_NOT_SUPPORTED 3

struct _WLHOST_LAUNCH
{
    WLREC_ACTION Action;
    WLUI_AUTH_ROLE Role;
    DWORD Scenario;
    GUID LaunchId;
    DWORD Pid;
    HANDLE Process;             /* held for the life of the entry: stops PID reuse */
    PWLUI_LAUNCH_STATE State;
    HANDLE Supervisor;
    HANDLE CancelEvent;
    HANDLE EndedEvent;
    WLREC_END End;
    DWORD ExitCode;
    BOOL Terminated;
};

/* The level machine is touched by the SAS thread and by every supervisor thread. */
static WLREC_STATE g_Recovery;
static CRITICAL_SECTION g_RecoveryLock;
static WLHOST_ENDED_CALLBACK g_EndedCallback;

static WLREC_ACTION
WlHostRecNext(VOID)
{
    WLREC_ACTION Action;
    EnterCriticalSection(&g_RecoveryLock);
    Action = WlRecNextAction(&g_Recovery);
    LeaveCriticalSection(&g_RecoveryLock);
    return Action;
}

/* Records an end and returns the level name for the log line, under one lock hold. */
static PCWSTR
WlHostRecEnd(
    _In_ WLREC_END End)
{
    PCWSTR Name;
    EnterCriticalSection(&g_RecoveryLock);
    WlRecOnEnd(&g_Recovery, End);
    Name = WlRecLevelName(WlRecLevel(&g_Recovery));
    LeaveCriticalSection(&g_RecoveryLock);
    return Name;
}

/* ---- event log ------------------------------------------------------------- */

VOID
WlHostLog(
    _In_ WORD Type,
    _In_ PCWSTR Text)
{
    HANDLE Source = RegisterEventSourceW(NULL, L"Winlogon");

    if (Type == EVENTLOG_ERROR_TYPE)
        ERR("WL: %ls\n", Text);
    else
        TRACE("WL: %ls\n", Text);

    if (Source != NULL)
    {
        /* Event 4001: logon UI supervision; the text carries the detail. */
        ReportEventW(Source, Type, 0, 4001, NULL, 1, 0, &Text, NULL);
        DeregisterEventSource(Source);
    }
}

/* ---- launching --------------------------------------------------------------- */

VOID
WlHostInit(VOID)
{
    InitializeCriticalSection(&g_RecoveryLock);
    WlRecInit(&g_Recovery, NULL);
}

VOID
WlHostSetEndedCallback(
    _In_opt_ WLHOST_ENDED_CALLBACK Callback)
{
    g_EndedCallback = Callback;
}

VOID
WlHostOnSas(VOID)
{
    EnterCriticalSection(&g_RecoveryLock);
    WlRecOnSas(&g_Recovery);
    LeaveCriticalSection(&g_RecoveryLock);
}

/* N4 reached: nothing launches again until a real SAS (the N4 dialog is a later commit). */
BOOL
WlHostIsTerminal(VOID)
{
    BOOL Terminal;

    EnterCriticalSection(&g_RecoveryLock);
    Terminal = (WlRecLevel(&g_Recovery) == WlRecLevel4);
    LeaveCriticalSection(&g_RecoveryLock);
    return Terminal;
}

static BOOL
WlHostBuildPath(
    _In_ PCWSTR Image,
    _Out_writes_(MAX_PATH) PWSTR Path)
{
    UINT Length = GetSystemDirectoryW(Path, MAX_PATH);
    if (Length == 0 || Length >= MAX_PATH)
        return FALSE;
    return SUCCEEDED(StringCchCatW(Path, MAX_PATH, L"\\")) &&
           SUCCEEDED(StringCchCatW(Path, MAX_PATH, Image));
}

static DWORD WINAPI WlHostSupervisor(_In_ LPVOID Parameter);

/* One launch at the given tier: verify the image, create suspended, register, resume. */
static PWLHOST_LAUNCH
WlHostLaunchOnce(
    _In_ WLREC_ACTION Action,
    _In_ DWORD Scenario,
    _Out_ WLREC_END *Failure)
{
    PWLHOST_LAUNCH Launch;
    WCHAR Path[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 128];
    WCHAR Text[MAX_PATH + 64];
    RPC_WSTR IdString = NULL;
    BYTE SystemSid[SECURITY_MAX_SID_SIZE];
    DWORD SidSize = sizeof(SystemSid);
    HANDLE Image = INVALID_HANDLE_VALUE;
    WLIMG_STATUS ImgStatus;
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    GUID Registered;
    BOOL Fallback = (Action == WlRecActLaunchHostFallback);

    *Failure = WlRecEndLaunchFailed;

    Launch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Launch));
    if (Launch == NULL)
        return NULL;
    Launch->Action = Action;
    Launch->Scenario = Scenario;
    Launch->Role = (Action == WlRecActLaunchRecovery) ? WluiAuthRoleRecovery : WluiAuthRoleHost;
    Launch->CancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Launch->EndedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Launch->CancelEvent == NULL || Launch->EndedEvent == NULL)
        goto Fail;

    if (!WlHostBuildPath(Launch->Role == WluiAuthRoleRecovery ? WLHOST_RECOVERY_EXE : WLHOST_LOGONUI_EXE, Path))
        goto Fail;

    /* §8.9 structural verification; the handle stays open until CreateProcess has returned (P5). */
    ImgStatus = WlImgOpenVerified(Path, WLIMG_REQUIRE_SYSTEM32 | WLIMG_CHECK_DIRECTORY, &Image);
    if (ImgStatus != WlImgOk)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI image %s rejected: %s (%lu)",
                         Path, WlImgStatusName(ImgStatus), GetLastError());
        WlHostLog(EVENTLOG_ERROR_TYPE, Text);
        *Failure = WlRecEndImageInvalid;
        goto Fail;
    }

    /* The id goes on the command line before the process exists; it is not a secret,
     * the PID and the SID are what authorize the child's calls. */
    if (UuidCreate(&Launch->LaunchId) != RPC_S_OK || UuidToStringW(&Launch->LaunchId, &IdString) != RPC_S_OK)
        goto Fail;
    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" /launchid:{%s} /scenario:%lu%s",
                     Path, (PCWSTR)IdString, Scenario, Fallback ? L" /fallback" : L"");
    RpcStringFreeW(&IdString);

    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Si.lpDesktop = L"WinSta0\\Winlogon";
    ZeroMemory(&Pi, sizeof(Pi));

    /* Suspended: the child must be registered before it can make its first call. */
    if (!CreateProcessW(Path, CommandLine, NULL, NULL, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &Si, &Pi))
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI launch failed: %s (%lu)", Path, GetLastError());
        WlHostLog(EVENTLOG_ERROR_TYPE, Text);
        goto Fail;
    }
    CloseHandle(Image);
    Image = INVALID_HANDLE_VALUE;

    Launch->Pid = Pi.dwProcessId;
    Launch->Process = Pi.hProcess;

    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, SystemSid, &SidSize))
        goto FailProcess;
    Launch->State = WluiLaunchRegister(Launch->Pid, (PSID)SystemSid, Launch->Role, &Launch->LaunchId, &Registered);
    if (Launch->State == NULL)
        goto FailProcess;

    Launch->Supervisor = CreateThread(NULL, 0, WlHostSupervisor, Launch, 0, NULL);
    if (Launch->Supervisor == NULL)
        goto FailProcess;

    ResumeThread(Pi.hThread);
    CloseHandle(Pi.hThread);

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI launched: %s pid %lu scenario %lu%s",
                     Path, Launch->Pid, Scenario, Fallback ? L" (fallback)" : L"");
    WlHostLog(EVENTLOG_INFORMATION_TYPE, Text);
    return Launch;

FailProcess:
    TerminateProcess(Pi.hProcess, 1);
    CloseHandle(Pi.hThread);
    if (Launch->State)
    {
        WluiLaunchUnregister(Launch->State);
        Launch->State = NULL;
    }
Fail:
    if (Image != INVALID_HANDLE_VALUE) CloseHandle(Image);
    if (Launch->Process) CloseHandle(Launch->Process);
    if (Launch->CancelEvent) CloseHandle(Launch->CancelEvent);
    if (Launch->EndedEvent) CloseHandle(Launch->EndedEvent);
    HeapFree(GetProcessHeap(), 0, Launch);
    return NULL;
}

/* 8.10 c: a policy that demands a stronger credential than a password closes the
 * password-only tiers (N2, N3): scforceoption, the PasswordProvider on the exclusion
 * list, or ProhibitFallbacks (D-b). The N1 host applies the same policy by itself. */
static BOOL
WlHostPolicyDeniesWeakTier(VOID)
{
    static const WCHAR PasswordClsid[] = L"{60B78E88-EAD8-445C-9CFD-0B87F74EA6CD}";
    DWORD Value = 0, Size = sizeof(Value), Type;
    PWSTR List;
    HKEY Key;
    BOOL Denied = FALSE;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"scforceoption", RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS && Value != 0)
        return TRUE;
    Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                     L"ProhibitFallbacks", RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS && Value != 0)
        return TRUE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", 0, KEY_QUERY_VALUE, &Key)
        != ERROR_SUCCESS)
        return FALSE;
    Size = 0;
    if (RegQueryValueExW(Key, L"ExcludedCredentialProviders", NULL, &Type, NULL, &Size) == ERROR_SUCCESS &&
        (Type == REG_SZ || Type == REG_EXPAND_SZ) && Size != 0)
    {
        List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + sizeof(WCHAR));
        if (List && RegQueryValueExW(Key, L"ExcludedCredentialProviders", NULL, &Type, (PBYTE)List, &Size) == ERROR_SUCCESS)
        {
            CharUpperW(List);
            Denied = wcsstr(List, PasswordClsid) != NULL;
        }
        if (List)
            HeapFree(GetProcessHeap(), 0, List);
    }
    RegCloseKey(Key);
    return Denied;
}

static BOOL
WlHostImagePresent(
    _In_ PCWSTR Image)
{
    WCHAR Path[MAX_PATH];
    return WlHostBuildPath(Image, Path) && GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

/*
 * Walks the levels until a tier launches or the machine is terminal. On NULL
 * with *Terminal TRUE the caller shows the N4 state; nothing is left to launch,
 * and *Reason says why (levels spent, both images missing, or policy).
 */
PWLHOST_LAUNCH
WlHostStart(
    _In_ DWORD Scenario,
    _Out_ PBOOL Terminal,
    _Out_ WLHOST_TERMINAL *Reason)
{
    *Terminal = FALSE;
    *Reason = WlHostTerminalNone;

    for (;;)
    {
        WLREC_ACTION Action = WlHostRecNext();
        WLREC_END Failure;
        PWLHOST_LAUNCH Launch;
        WCHAR Text[96];

        if (Action == WlRecActTerminal)
        {
            BOOL Missing = !WlHostImagePresent(WLHOST_LOGONUI_EXE) && !WlHostImagePresent(WLHOST_RECOVERY_EXE);
            WlHostLog(EVENTLOG_ERROR_TYPE, Missing
                      ? L"logon UI recovery reached the terminal state (N4): both logon UI images are missing"
                      : L"logon UI recovery reached the terminal state (N4)");
            *Terminal = TRUE;
            *Reason = Missing ? WlHostTerminalMissing : WlHostTerminalLevels;
            return NULL;
        }

        if (Action != WlRecActLaunchHost && WlHostPolicyDeniesWeakTier())
        {
            /* The levels are left where they are: a later SAS may retry N1 as usual. */
            WlHostLog(EVENTLOG_ERROR_TYPE, L"logon UI password tier refused by policy (scforceoption, exclusion or ProhibitFallbacks)");
            *Terminal = TRUE;
            *Reason = WlHostTerminalPolicy;
            return NULL;
        }

        Launch = WlHostLaunchOnce(Action, Scenario, &Failure);
        if (Launch != NULL)
            return Launch;

        StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI tier could not start (%s); now at %s",
                         WlRecEndName(Failure), WlHostRecEnd(Failure));
        WlHostLog(EVENTLOG_WARNING_TYPE, Text);
    }
}

/* ---- supervision --------------------------------------------------------------- */

static DWORD WINAPI
WlHostSupervisor(
    _In_ LPVOID Parameter)
{
    PWLHOST_LAUNCH Launch = Parameter;
    HANDLE Waits[2] = { Launch->Process, Launch->CancelEvent };
    ULONGLONG Started = GetTickCount64();
    WLREC_END End = WlRecEndNormal;
    DWORD ExitCode = 0;
    WCHAR Text[96];

    for (;;)
    {
        DWORD Which = WaitForMultipleObjects(2, Waits, FALSE, 1000);
        ULONGLONG Now, Silence;

        if (Which == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(Launch->Process, &ExitCode);
            if (Launch->Terminated)
                End = WlRecEndNormal;           /* we ended it ourselves */
            else if (ExitCode == WLHOST_EXIT_OK && WluiLaunchExited(Launch->State, NULL))
                End = WlRecEndNormal;           /* a bare exit 0 without Exiting is a crash */
            else if (ExitCode == WLHOST_EXIT_NOT_SUPPORTED)
                End = WlRecEndProvidersMissing; /* the host could not bring up a credential tier */
            else
                End = WlRecEndCrash;
            break;
        }
        if (Which == WAIT_OBJECT_0 + 1)
        {
            End = WlRecEndNormal;               /* cancelled by WlHostStop */
            break;
        }

        /* Hang: the UI thread stopped beating. Before Ready a host may still be loading. */
        Now = GetTickCount64();
        Silence = Now - WluiLaunchLastHeartbeat(Launch->State);
        if (Silence > WLHOST_HANG_MS &&
            (WluiLaunchWaitReady(Launch->State, 0) || Now - Started > WLHOST_STARTUP_MS))
        {
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI pid %lu hung (%I64u ms without a heartbeat)",
                             Launch->Pid, Silence);
            WlHostLog(EVENTLOG_ERROR_TYPE, Text);
            Launch->Terminated = TRUE;
            TerminateProcess(Launch->Process, 2);
            WaitForSingleObject(Launch->Process, 2000);
            GetExitCodeProcess(Launch->Process, &ExitCode);
            End = WlRecEndHang;
            break;
        }
    }

    Launch->End = End;
    Launch->ExitCode = ExitCode;
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"logon UI pid %lu ended: %s (exit %lu), level %s",
                     Launch->Pid, WlRecEndName(End), ExitCode,
                     /* an end winlogon caused is a normal end for the budget: it only resets the count */
                     (!Launch->Terminated || End == WlRecEndHang) ? WlHostRecEnd(End) : WlHostRecEnd(WlRecEndNormal));
    WlHostLog(End == WlRecEndNormal ? EVENTLOG_INFORMATION_TYPE : EVENTLOG_WARNING_TYPE, Text);
    SetEvent(Launch->EndedEvent);
    /* WlHostStop joins this thread before it frees the entry, so Launch is still valid here. */
    if (g_EndedCallback)
        g_EndedCallback(Launch);
    return 0;
}

/* ---- what the SAS flows use ------------------------------------------------------ */

BOOL
WlHostWaitReady(
    _In_ PWLHOST_LAUNCH Launch,
    _In_ DWORD TimeoutMs)
{
    return WluiLaunchWaitReady(Launch->State, TimeoutMs);
}

BOOL
WlHostPost(
    _In_ PWLHOST_LAUNCH Launch,
    _In_ const struct _WLUI_COMMAND *Command)
{
    return WluiLaunchPostCommand(Launch->State, Command);
}

HANDLE
WlHostEndedEvent(
    _In_ PWLHOST_LAUNCH Launch)
{
    return Launch->EndedEvent;
}

BOOL
WlHostHasEnded(
    _In_ PWLHOST_LAUNCH Launch,
    _Out_opt_ WLREC_END *End,
    _Out_opt_ PDWORD ExitCode)
{
    if (WaitForSingleObject(Launch->EndedEvent, 0) != WAIT_OBJECT_0)
        return FALSE;
    if (End) *End = Launch->End;
    if (ExitCode) *ExitCode = Launch->ExitCode;
    return TRUE;
}

PWLUI_LAUNCH_STATE
WlHostState(
    _In_ PWLHOST_LAUNCH Launch)
{
    return Launch->State;
}

/* Dismisses the UI, waits briefly, terminates what is left, then releases the entry. */
VOID
WlHostStop(
    _In_ PWLHOST_LAUNCH Launch)
{
    WLUI_COMMAND Dismiss;

    if (WaitForSingleObject(Launch->EndedEvent, 0) != WAIT_OBJECT_0)
    {
        ZeroMemory(&Dismiss, sizeof(Dismiss));
        Dismiss.Type = WluiCmdDismiss;
        WluiLaunchPostCommand(Launch->State, &Dismiss);
        if (WaitForSingleObject(Launch->Process, WLHOST_DISMISS_MS) != WAIT_OBJECT_0)
        {
            Launch->Terminated = TRUE;
            TerminateProcess(Launch->Process, 3);
        }
        SetEvent(Launch->CancelEvent);
    }
    if (Launch->Supervisor)
    {
        /* The supervisor may be inside an event-log RPC; freeing under it would be a
         * use-after-free, and winlogon must never wait on the event log. Leak instead. */
        if (WaitForSingleObject(Launch->Supervisor, WLHOST_SUPERVISOR_MS) != WAIT_OBJECT_0)
        {
            ERR("WL: logon UI pid %lu supervisor still busy after %u ms; entry leaked\n",
                Launch->Pid, WLHOST_SUPERVISOR_MS);
            return;
        }
        CloseHandle(Launch->Supervisor);
    }

    /* Unregistering wakes a blocked WaitForCommand with 995 and frees the id; the
     * process handle goes last so the PID could not be reused while the entry lived. */
    WluiLaunchUnregister(Launch->State);
    CloseHandle(Launch->Process);
    CloseHandle(Launch->CancelEvent);
    CloseHandle(Launch->EndedEvent);
    SecureZeroMemory(Launch, sizeof(*Launch));
    HeapFree(GetProcessHeap(), 0, Launch);
}
