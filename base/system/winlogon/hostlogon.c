/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host-mode logon: what msgina did for the logged-out SAS, done by
 *              winlogon with the LogonUI host (PHASE3-PLAN §3 commit 6, §10.3).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#include <rpc.h>
#include <ntsecapi.h>
#include <credser.h>
#include <wlui_s.h>

#define WLHL_READY_MS       2000
#define WLHL_FADE_WAIT_MS   1500
#define WLHL_CPUS_LOGON     1       /* CPUS_LOGON */

typedef struct _WLHL_SUBMIT
{
    BOOL Pending;
    NTSTATUS Status;
    NTSTATUS SubStatus;
    HANDLE Token;
    LUID LogonId;
    WCHAR User[CREDSER_MAX_NAME_CHARS + 1];
    WCHAR Domain[CREDSER_MAX_NAME_CHARS + 1];
    WCHAR Password[CREDSER_MAX_PASSWORD_CHARS + 1];
} WLHL_SUBMIT;

static HANDLE g_Lsa;
static ULONG g_MsvPackage;
static CREDSER_PACKAGES g_Packages;
static BOOL g_Connected;
static CRITICAL_SECTION g_SubmitLock;
static HANDLE g_SubmitEvent;
static WLHL_SUBMIT g_Submit;
static DWORD g_PendingAction;           /* WLUI_ACTION from the host's power flyout */
static PWLHOST_LAUNCH g_Launch;         /* the live host, if any */
static BOOL g_Accepting;                /* the SAS thread is waiting for a submit (R2) */
static BOOL g_SubmitBusy;               /* a submit is between the gate and the slot */

/* ---- LSA ---------------------------------------------------------------------- */

static BOOL
WlHlConnectLsa(VOID)
{
    LSA_STRING Name;
    LSA_OPERATIONAL_MODE Mode;
    NTSTATUS Status;
    BOOLEAN Old;

    if (g_Connected)
        return TRUE;

    RtlInitString(&Name, "Winlogon");
    Status = LsaRegisterLogonProcess(&Name, &g_Lsa, &Mode);
    if (Status == STATUS_PORT_CONNECTION_REFUSED)
    {
        RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old);
        Status = LsaRegisterLogonProcess(&Name, &g_Lsa, &Mode);
    }
    if (!NT_SUCCESS(Status))
    {
        ERR("WL: LsaRegisterLogonProcess failed 0x%08lx\n", Status);
        return FALSE;
    }

    Status = CredSerLookupPackages(g_Lsa, &g_Packages);
    if (!NT_SUCCESS(Status) || !g_Packages.HaveMsv1_0)
    {
        ERR("WL: no MSV1_0 package (0x%08lx)\n", Status);
        LsaDeregisterLogonProcess(g_Lsa);
        g_Lsa = NULL;
        return FALSE;
    }
    g_MsvPackage = g_Packages.Msv1_0;

    InitializeCriticalSection(&g_SubmitLock);
    g_SubmitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_Connected = (g_SubmitEvent != NULL);
    return g_Connected;
}

/* A provider CLSID is acceptable when it is registered for logon on this machine. */
static BOOL
WlHlProviderRegistered(
    _In_ const GUID *Clsid)
{
    static const GUID PasswordProvider =
        { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } };
    WCHAR Key[160], Text[40];
    HKEY hKey;

    if (IsEqualGUID(Clsid, &PasswordProvider))
        return TRUE;
    if (StringFromGUID2(Clsid, Text, ARRAYSIZE(Text)) == 0)
        return FALSE;
    StringCchPrintfW(Key, ARRAYSIZE(Key),
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\%s", Text);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Key, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

static VOID
WlHlCopyString(
    _In_ const UNICODE_STRING *Source,
    _Out_writes_(Chars) PWSTR Dest,
    _In_ SIZE_T Chars)
{
    SIZE_T Count = Source->Length / sizeof(WCHAR);
    if (Count >= Chars)
        Count = Chars - 1;
    CopyMemory(Dest, Source->Buffer, Count * sizeof(WCHAR));
    Dest[Count] = UNICODE_NULL;
}

/* msgina lsa.c MyLogonUser: lsasrv adds no logon SID itself and AllowAccessOnSession needs one. */
static PTOKEN_GROUPS
WlHlBuildLogonGroups(VOID)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    SID_IDENTIFIER_AUTHORITY LocalAuthority = { SECURITY_LOCAL_SID_AUTHORITY };
    PSID LogonSid = NULL, LocalSid = NULL;
    PTOKEN_GROUPS Groups = NULL;
    LUID LogonLuid;

    AllocateLocallyUniqueId(&LogonLuid);
    if (!AllocateAndInitializeSid(&NtAuthority, SECURITY_LOGON_IDS_RID_COUNT, SECURITY_LOGON_IDS_RID,
                                  LogonLuid.HighPart, LogonLuid.LowPart, 0, 0, 0, 0, 0, &LogonSid) ||
        !AllocateAndInitializeSid(&LocalAuthority, 1, SECURITY_LOCAL_RID, 0, 0, 0, 0, 0, 0, 0, &LocalSid) ||
        (Groups = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, FIELD_OFFSET(TOKEN_GROUPS, Groups[2]))) == NULL)
    {
        if (LogonSid) FreeSid(LogonSid);
        if (LocalSid) FreeSid(LocalSid);
        return NULL;
    }
    Groups->GroupCount = 2;
    Groups->Groups[0].Sid = LogonSid;
    Groups->Groups[0].Attributes = SE_GROUP_MANDATORY | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_LOGON_ID;
    Groups->Groups[1].Sid = LocalSid;
    Groups->Groups[1].Attributes = SE_GROUP_MANDATORY | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT;
    return Groups;
}

static VOID
WlHlFreeLogonGroups(
    _In_opt_ PTOKEN_GROUPS Groups)
{
    if (Groups == NULL)
        return;
    FreeSid(Groups->Groups[0].Sid);
    FreeSid(Groups->Groups[1].Sid);
    LocalFree(Groups);
}

static BOOL
WlHlTokenHasLogonSid(
    _In_ HANDLE Token)
{
    PTOKEN_GROUPS Groups;
    DWORD Size = 0, i;
    BOOL Found = FALSE;

    GetTokenInformation(Token, TokenGroups, NULL, 0, &Size);
    Groups = LocalAlloc(LMEM_FIXED, Size);
    if (Groups == NULL || !GetTokenInformation(Token, TokenGroups, Groups, Size, &Size))
    {
        LocalFree(Groups);
        return FALSE;
    }
    for (i = 0; i < Groups->GroupCount && !Found; i++)
        Found = (Groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != 0;
    LocalFree(Groups);
    return Found;
}

/* Runs on an RPC thread: the serialization is untrusted input (PHASE3-PLAN §5.1, Q1). */
static DWORD
WlHlSubmitHandler(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ WLUI_AUTH_ROLE Role,
    _In_ DWORD Scenario,
    _In_ const GUID *ProviderClsid,
    _In_ DWORD AuthenticationPackage,
    _In_reads_bytes_(cbSerialization) const BYTE *Serialization,
    _In_ DWORD cbSerialization)
{
    CREDSER_LOGON Logon;
    PMSV1_0_INTERACTIVE_LOGON Msv = NULL;
    PTOKEN_GROUPS Groups;
    ULONG MsvSize = 0, ProfileSize = 0;
    PVOID Copy = NULL, Profile = NULL;
    LSA_STRING Origin;
    TOKEN_SOURCE Source;
    QUOTA_LIMITS Quotas;
    LUID LogonId;
    HANDLE Token = NULL;
    NTSTATUS Status, SubStatus = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(Role);

    if (Scenario != WLHL_CPUS_LOGON)
        return ERROR_NOT_SUPPORTED;         /* unlock and change-password land with the next commit */
    if (!CredSerIsKnownPackage(&g_Packages, AuthenticationPackage))
        return ERROR_INVALID_PARAMETER;
    if (!WlHlProviderRegistered(ProviderClsid))
        return ERROR_ACCESS_DENIED;
    if (Serialization == NULL || cbSerialization == 0 || cbSerialization > CREDSER_MAX_BLOB_SIZE)
        return ERROR_INVALID_PARAMETER;

    /* A logon runs only while the SAS thread waits for one, and one at a time (R2). */
    EnterCriticalSection(&g_SubmitLock);
    if (!g_Accepting || g_SubmitBusy || g_Submit.Pending)
    {
        LeaveCriticalSection(&g_SubmitLock);
        return ERROR_BUSY;
    }
    g_SubmitBusy = TRUE;
    LeaveCriticalSection(&g_SubmitLock);

    /* One private copy; every later step reads only the copy. */
    Status = CredSerCapture(Serialization, cbSerialization, &Copy);
    if (NT_SUCCESS(Status))
        Status = CredSerUnpackLogon(Copy, cbSerialization, &Logon);
    if (NT_SUCCESS(Status))
        Status = CredSerBuildMsv1_0Logon(&Logon, &Msv, &MsvSize);
    if (!NT_SUCCESS(Status))
    {
        CredSerFree(Copy, cbSerialization);
        EnterCriticalSection(&g_SubmitLock);
        g_SubmitBusy = FALSE;
        LeaveCriticalSection(&g_SubmitLock);
        return ERROR_INVALID_PARAMETER;
    }

    RtlInitString(&Origin, "Winlogon");
    CopyMemory(Source.SourceName, "User32  ", TOKEN_SOURCE_LENGTH);
    AllocateLocallyUniqueId(&Source.SourceIdentifier);

    Groups = WlHlBuildLogonGroups();
    Status = Groups ? LsaLogonUser(g_Lsa, &Origin, Interactive, g_MsvPackage, Msv, MsvSize, Groups, &Source,
                                   &Profile, &ProfileSize, &LogonId, &Token, &Quotas, &SubStatus)
                    : (NTSTATUS)STATUS_NO_MEMORY;
    WlHlFreeLogonGroups(Groups);
    if (Profile)
        LsaFreeReturnBuffer(Profile);
    if (NT_SUCCESS(Status) && !WlHlTokenHasLogonSid(Token))
        ERR("WL: logon token carries no logon SID; AllowAccessOnSession will refuse it\n");

    EnterCriticalSection(&g_SubmitLock);
    g_SubmitBusy = FALSE;
    if (!g_Accepting)
    {
        /* The SAS thread stopped waiting meanwhile: nobody would ever consume this logon. */
        LeaveCriticalSection(&g_SubmitLock);
        if (Token)
            CloseHandle(Token);
        CredSerFree(Msv, MsvSize);
        CredSerFree(Copy, cbSerialization);
        return ERROR_BUSY;
    }
    SecureZeroMemory(&g_Submit, sizeof(g_Submit));
    g_Submit.Pending = TRUE;
    g_Submit.Status = Status;
    g_Submit.SubStatus = SubStatus;
    if (NT_SUCCESS(Status))
    {
        g_Submit.Token = Token;
        g_Submit.LogonId = LogonId;
        WlHlCopyString(&Logon.UserName, g_Submit.User, ARRAYSIZE(g_Submit.User));
        WlHlCopyString(&Logon.LogonDomainName, g_Submit.Domain, ARRAYSIZE(g_Submit.Domain));
        /* The network-provider notification (MPR) still wants the password; it is wiped after use. */
        WlHlCopyString(&Logon.Password, g_Submit.Password, ARRAYSIZE(g_Submit.Password));
    }
    LeaveCriticalSection(&g_SubmitLock);

    CredSerFree(Msv, MsvSize);
    CredSerFree(Copy, cbSerialization);
    SetEvent(g_SubmitEvent);
    return ERROR_SUCCESS;
}

static DWORD
WlHlActionHandler(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ WLUI_AUTH_ROLE Role,
    _In_ DWORD Action)
{
    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(Role);

    if (Action != WluiActionShutdown && Action != WluiActionRestart && Action != WluiActionCancel)
        return ERROR_INVALID_PARAMETER;
    g_PendingAction = Action;
    SetEvent(g_SubmitEvent);
    return ERROR_SUCCESS;
}

/* ---- the logged-out SAS ------------------------------------------------------- */

static BOOL
WlHlDisableCad(VOID)
{
    DWORD Value = 0, Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                     L"DisableCAD", RRF_RT_REG_DWORD, NULL, &Value, &Size) != ERROR_SUCCESS)
        Value = 0;
    return Value != 0;
}

static VOID
WlHlPost(
    _In_ WLUI_COMMAND_TYPE Type,
    _In_ DWORD Scenario,
    _In_ DWORD Flags,
    _In_ LONG Status,
    _In_ LONG SubStatus,
    _In_opt_ PCWSTR User,
    _In_opt_ PCWSTR Text)
{
    WLUI_COMMAND Command;

    if (g_Launch == NULL)
        return;
    ZeroMemory(&Command, sizeof(Command));
    Command.Type = Type;
    Command.Scenario = Scenario;
    Command.Flags = Flags;
    Command.Status = Status;
    Command.SubStatus = SubStatus;
    if (User) StringCchCopyW(Command.User, ARRAYSIZE(Command.User), User);
    if (Text) StringCchCopyW(Command.Text, ARRAYSIZE(Command.Text), Text);
    WlHostPost(g_Launch, &Command);
}

/* Brings up a host asking for logon credentials, reusing a live one. */
static BOOL
WlHlEnsureHost(VOID)
{
    BOOL Terminal = FALSE;

    if (g_Launch != NULL && !WlHostHasEnded(g_Launch, NULL, NULL))
        return TRUE;
    if (g_Launch != NULL)
    {
        WlHostStop(g_Launch);
        g_Launch = NULL;
    }

    g_Launch = WlHostStart(WLHL_CPUS_LOGON, &Terminal);
    if (g_Launch == NULL)
        return FALSE;                   /* N4: the terminal state is drawn by a later commit */

    if (!WlHostWaitReady(g_Launch, WLHL_READY_MS))
        WARN("WL: logon UI not ready after %u ms, requesting credentials anyway\n", WLHL_READY_MS);
    WlHlPost(WluiCmdRequestCredentials, WLHL_CPUS_LOGON, WlHlDisableCad() ? WLUI_FLAG_DISABLE_CAD : 0, 0, 0, NULL, NULL);
    return TRUE;
}

/* While accepting, submits may park a logon in the slot; afterwards a leftover one is closed. */
static VOID
WlHlSetAccepting(
    _In_ BOOL Accepting)
{
    EnterCriticalSection(&g_SubmitLock);
    g_Accepting = Accepting;
    if (!Accepting)
    {
        if (g_Submit.Token)
            CloseHandle(g_Submit.Token);
        SecureZeroMemory(&g_Submit, sizeof(g_Submit));
    }
    LeaveCriticalSection(&g_SubmitLock);
}

/* With CAD disabled the credential page is the notice: enter the logged-out SAS by ourselves. */
VOID
WlHostDisplaySasNotice(
    _In_ PWLSESSION Session)
{
    if (!WlHlDisableCad())
        return;
    if (WlHostIsTerminal())
    {
        /* N4: posting again would spin; a real SAS is the only way on until the dialog of commit 10. */
        ERR("WL: logon UI is in its terminal state; waiting for a user SAS\n");
        return;
    }
    Session->HostAutoSas = TRUE;
    PostMessageW(Session->SASWindow, WLX_WM_SAS, WLX_SAS_TYPE_CTRL_ALT_DEL, 0);
}

DWORD
WlHostLoggedOutSas(
    _In_ PWLSESSION Session)
{
    HANDLE Waits[2];
    BOOL UserSas = !Session->HostAutoSas;
    DWORD Result = WLX_SAS_ACTION_NONE;

    Session->HostAutoSas = FALSE;
    if (!WlHlConnectLsa())
        return WLX_SAS_ACTION_NONE;
    WluiSetSubmitHandler(WlHlSubmitHandler);
    WluiSetActionHandler(WlHlActionHandler);

    /* Only a SAS the user typed resets the levels; the self-posted one must not (R3). */
    if (UserSas)
        WlHostOnSas();
    if (!WlHlEnsureHost())
        return WLX_SAS_ACTION_NONE;
    WlHlSetAccepting(TRUE);

    for (;;)
    {
        DWORD Which;

        Waits[0] = g_SubmitEvent;
        Waits[1] = WlHostEndedEvent(g_Launch);
        Which = WaitForMultipleObjects(2, Waits, FALSE, INFINITE);

        if (Which == WAIT_OBJECT_0 + 1)
        {
            /* The host died or hung; the level machine already chose the next tier. */
            if (!WlHlEnsureHost())
                break;
            continue;
        }

        if (g_PendingAction != 0)
        {
            DWORD Action = g_PendingAction;
            g_PendingAction = 0;
            if (Action == WluiActionShutdown)
            {
                Result = WLX_SAS_ACTION_SHUTDOWN_POWER_OFF;
                break;
            }
            if (Action == WluiActionRestart)
            {
                Result = WLX_SAS_ACTION_SHUTDOWN_REBOOT;
                break;
            }
            continue;
        }

        EnterCriticalSection(&g_SubmitLock);
        if (!g_Submit.Pending)
        {
            LeaveCriticalSection(&g_SubmitLock);
            continue;
        }
        g_Submit.Pending = FALSE;

        if (!NT_SUCCESS(g_Submit.Status))
        {
            NTSTATUS Status = g_Submit.Status, SubStatus = g_Submit.SubStatus;
            LeaveCriticalSection(&g_SubmitLock);
            /* The provider owns the wording of its result (STATUS_LOGON_FAILURE and friends). */
            WlHlPost(WluiCmdReportResult, WLHL_CPUS_LOGON, 0, Status, SubStatus, NULL, NULL);
            continue;
        }

        /* Fill the session the way the GINA's WlxLoggedOutSAS did (§10.3). */
        Session->UserToken = g_Submit.Token;
        Session->LogonId = g_Submit.LogonId;
        Session->Options = 0;
        ZeroMemory(&Session->MprNotifyInfo, sizeof(Session->MprNotifyInfo));
        Session->MprNotifyInfo.pszUserName = WlStrDup(g_Submit.User);
        Session->MprNotifyInfo.pszDomain = WlStrDup(g_Submit.Domain);
        Session->MprNotifyInfo.pszPassword = WlStrDup(g_Submit.Password);
        Session->MprNotifyInfo.pszOldPassword = NULL;
        Session->Profile = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(WLX_PROFILE_V2_0));
        if (Session->Profile)
            Session->Profile->dwType = WLX_PROFILE_TYPE_V2_0;   /* local account: default paths */
        SecureZeroMemory(g_Submit.Password, sizeof(g_Submit.Password));
        g_Submit.Token = NULL;
        LeaveCriticalSection(&g_SubmitLock);

        WlHlPost(WluiCmdDisplayWelcome, WLHL_CPUS_LOGON, WLUI_FLAG_SPINNER, 0, 0, Session->MprNotifyInfo.pszUserName, NULL);
        Result = WLX_SAS_ACTION_LOGON;
        break;
    }

    WlHlSetAccepting(FALSE);
    return Result;
}

/* ---- status and hand-offs used by HandleLogon -------------------------------- */

VOID
WlHostStatus(
    _In_ PCWSTR Text)
{
    WlHlPost(WluiCmdDisplayStatus, WLHL_CPUS_LOGON, WLUI_FLAG_SPINNER, 0, 0, NULL, Text);
}

/* Theme service wait msgina did before starting the shell (shsvcs ordinal 2). */
VOID
WlHostThemeWait(VOID)
{
    typedef DWORD (WINAPI *THEMEWAIT)(DWORD Timeout);
    HMODULE Module = LoadLibraryW(L"shsvcs.dll");
    THEMEWAIT Wait;

    if (Module == NULL)
        return;
    Wait = (THEMEWAIT)GetProcAddress(Module, (LPCSTR)2);
    if (Wait)
        Wait(5000);
    FreeLibrary(Module);
}

/* The desktop is about to be switched: let the host fade, then give it a moment to leave. */
VOID
WlHostBeforeDesktopSwitch(VOID)
{
    if (g_Launch == NULL)
        return;
    WlHlPost(WluiCmdReadyForDesktopSwitch, WLHL_CPUS_LOGON, 0, 0, 0, NULL, NULL);
    WaitForSingleObject(WlHostEndedEvent(g_Launch), WLHL_FADE_WAIT_MS);
}

VOID
WlHostAfterDesktopSwitch(VOID)
{
    if (g_Launch != NULL)
    {
        WlHostStop(g_Launch);
        g_Launch = NULL;
    }
}

/* A logon that failed after authentication: tell the host, keep it for the next attempt. */
VOID
WlHostLogonFailed(
    _In_ NTSTATUS Status)
{
    WlHlPost(WluiCmdReportResult, WLHL_CPUS_LOGON, 0, Status, 0, NULL, NULL);
}

/* ---- WLX duties absorbed from msgina --------------------------------------------- */

static BOOL
WlHlRunAsUser(
    _In_ PWLSESSION Session,
    _In_ PCWSTR Desktop,
    _In_opt_ PVOID Environment,
    _In_ PCWSTR Command)
{
    WCHAR CommandLine[MAX_PATH * 2];
    WCHAR DesktopName[64];
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    BOOL Ok;

    StringCchCopyW(CommandLine, ARRAYSIZE(CommandLine), Command);
    StringCchPrintfW(DesktopName, ARRAYSIZE(DesktopName), L"WinSta0\\%s", Desktop);
    ZeroMemory(&Si, sizeof(Si));
    Si.cb = sizeof(Si);
    Si.lpDesktop = DesktopName;
    Ok = CreateProcessAsUserW(Session->UserToken, NULL, CommandLine, NULL, NULL, FALSE,
                              CREATE_UNICODE_ENVIRONMENT, Environment, NULL, &Si, &Pi);
    if (!Ok)
    {
        ERR("WL: CreateProcessAsUser(%ls) failed %lu\n", Command, GetLastError());
        return FALSE;
    }
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
    return TRUE;
}

BOOL
WlHostStartApplication(
    _In_ PWLSESSION Session,
    _In_ PCWSTR Desktop,
    _In_opt_ PVOID Environment,
    _In_ PCWSTR Command)
{
    return WlHlRunAsUser(Session, Desktop, Environment, Command);
}

/* Userinit is a comma-separated command list (msgina WlxActivateUserShell). */
BOOL
WlHostActivateUserShell(
    _In_ PWLSESSION Session,
    _In_opt_ PVOID Environment)
{
    WCHAR Raw[MAX_PATH + 1], Expanded[MAX_PATH * 2];
    DWORD Size = sizeof(Raw) - sizeof(UNICODE_NULL), Type;
    HKEY hKey;
    PWSTR Command, Next;
    BOOL Any = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(hKey, L"Userinit", NULL, &Type, (PBYTE)Raw, &Size) != ERROR_SUCCESS ||
        (Type != REG_SZ && Type != REG_EXPAND_SZ))
    {
        RegCloseKey(hKey);
        return FALSE;
    }
    RegCloseKey(hKey);
    Raw[Size / sizeof(WCHAR) < MAX_PATH ? Size / sizeof(WCHAR) : MAX_PATH] = UNICODE_NULL;  /* the value may lack its NUL */
    if (ExpandEnvironmentStringsW(Raw, Expanded, ARRAYSIZE(Expanded)) == 0)
        return FALSE;

    for (Command = Expanded; Command && *Command; Command = Next)
    {
        Next = wcschr(Command, L',');
        if (Next)
            *Next++ = UNICODE_NULL;
        while (*Command == L' ') Command++;
        if (*Command == UNICODE_NULL)
            continue;
        if (WlHlRunAsUser(Session, L"Default", Environment, Command))
            Any = TRUE;
    }
    return Any;
}
