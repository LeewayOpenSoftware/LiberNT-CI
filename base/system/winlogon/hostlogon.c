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
#include <ndk/setypes.h>
#include <credser.h>
#include <wlui_s.h>
#include <reactos/desktopready.h>

#define WLHL_READY_MS       2000
#define WLHL_HOST_EXIT_MS   3000    /* the host leaves on its own after its fade; then it is stopped */
#define WLHL_SWITCH_CAP_S   600     /* upper bound for DelayedDesktopSwitchTimeout */
#define WLHL_SHOWN_MS       1500    /* how long a switch waits for the host to paint its screen */
#define WLHL_LOCK_READY_MS  2000    /* default for HKLM Winlogon\LockReadyTimeoutMs (P2) */
#define WLHL_CPUS_LOGON             1   /* CPUS_LOGON */
#define WLHL_CPUS_UNLOCK            2   /* CPUS_UNLOCK_WORKSTATION */
#define WLHL_CPUS_CHANGE_PASSWORD   3   /* CPUS_CHANGE_PASSWORD */
#define WLHL_CHANGE_SIGNED_IN       0x200   /* g_ChangeFor: a change the signed-in user asked for from the options */
#define WLHL_ACTION_CHANGED         0x1000  /* g_PendingAction: that change went through (never from the host) */

#define WLHL_WINLOGON_KEY   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"
#define WLHL_POLICY_KEY     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"

typedef struct _WLHL_SUBMIT
{
    BOOL Pending;
    NTSTATUS Status;
    NTSTATUS SubStatus;
    BOOL MustChange;        /* the account may only continue through a password change */
    UINT TextId;            /* a refusal worded by winlogon (safe mode), 0 = provider wording */
    HANDLE Token;
    LUID LogonId;
    WCHAR User[CREDSER_MAX_NAME_CHARS + 1];
    WCHAR Domain[CREDSER_MAX_NAME_CHARS + 1];
    WCHAR Password[CREDSER_MAX_PASSWORD_CHARS + 1];
} WLHL_SUBMIT;

static PWLSESSION g_Session;
static BOOL g_HostLogonReady;           /* WlHostLogonInit ran: locks and event exist */
static HANDLE g_Lsa;
static ULONG g_MsvPackage;
static CREDSER_PACKAGES g_Packages;
static BOOL g_Connected;
static CRITICAL_SECTION g_SubmitLock;   /* g_Submit, g_PendingAction, g_Accepting, g_SubmitBusy, g_ChangeFor */
static CRITICAL_SECTION g_LaunchLock;   /* g_Launch: swapped by the SAS thread, read by RPC threads */
static HANDLE g_SubmitEvent;
static WLHL_SUBMIT g_Submit;
static DWORD g_PendingAction;           /* WLUI_ACTION from the host's power flyout */
static PWLHOST_LAUNCH g_Launch;         /* the live host, if any (SAS thread owns it) */
static DWORD g_HostScenario;            /* what the live host was asked to show; 0 = status only */
static BOOL g_Accepting;                /* the SAS thread is waiting for a logon submit (R2) */
static BOOL g_OptionsUp;                /* the SAS thread is waiting on the Ctrl+Alt+Del options */
static DWORD g_OptionsFlags;            /* WLUI_FLAG_NO_* the options page was shown with */
static BOOL g_SubmitBusy;               /* a submit is between the gate and the slot */
static DWORD g_ChangeFor;               /* scenario a forced password change resumes; 0 = no change page */
static BOOL g_LsaBroken;                /* the last logged-out SAS could not reach the LSA */
static HANDLE g_SwitchEvent;            /* ShellDesktopSwitchEvent of the logon in progress */
static DWORD g_StatusSequence;          /* the last status command; a switch waits for its paint */
static HANDLE g_ShellProcess;           /* first Userinit process of the logon in progress */
static HWND g_TerminalDialog;           /* the N4 dialog, while it stands (owned by its own thread) */
static HANDLE g_TerminalThread;         /* the thread that pumps that dialog */
static HANDLE g_TerminalShown;          /* set once the thread has created the dialog, or failed */
#define WM_LUT_CLOSE (WM_APP + 0x10)    /* the SAS thread asks the dialog thread to leave */

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
    g_Connected = TRUE;
    return TRUE;
}

static const GUID WlHlPasswordProvider =
    { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } };

/* A provider CLSID is acceptable when it is registered for logon on this machine. */
static BOOL
WlHlProviderRegistered(
    _In_ const GUID *Clsid)
{
    WCHAR Key[160], Text[40];
    HKEY hKey;

    if (IsEqualGUID(Clsid, &WlHlPasswordProvider))
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
    PSID_AND_ATTRIBUTES Entry;
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
    Entry = Groups->Groups;             /* the block holds two entries; the type declares one */
    Entry[0].Sid = LogonSid;
    Entry[0].Attributes = SE_GROUP_MANDATORY | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_LOGON_ID;
    Entry[1].Sid = LocalSid;
    Entry[1].Attributes = SE_GROUP_MANDATORY | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT;
    return Groups;
}

static VOID
WlHlFreeLogonGroups(
    _In_opt_ PTOKEN_GROUPS Groups)
{
    PSID_AND_ATTRIBUTES Entry;

    if (Groups == NULL)
        return;
    Entry = Groups->Groups;
    FreeSid(Entry[0].Sid);
    FreeSid(Entry[1].Sid);
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

/* Q2: an unlock is decided by the identity of the session's token, never by the blob. */
static BOOL
WlHlSameTokenUser(
    _In_opt_ HANDLE A,
    _In_opt_ HANDLE B)
{
    BYTE BufA[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE], BufB[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD Size;

    if (A == NULL || B == NULL)
        return FALSE;
    if (!GetTokenInformation(A, TokenUser, BufA, sizeof(BufA), &Size) ||
        !GetTokenInformation(B, TokenUser, BufB, sizeof(BufB), &Size))
        return FALSE;
    return EqualSid(((PTOKEN_USER)BufA)->User.Sid, ((PTOKEN_USER)BufB)->User.Sid);
}

static BOOL
WlHlMustChange(
    _In_ NTSTATUS Status,
    _In_ NTSTATUS SubStatus)
{
    return Status == STATUS_PASSWORD_MUST_CHANGE || Status == STATUS_PASSWORD_EXPIRED ||
           SubStatus == STATUS_PASSWORD_MUST_CHANGE || SubStatus == STATUS_PASSWORD_EXPIRED;
}

/* msgina IsAdminToken: BUILTIN\Administrators among the token's groups. */
static BOOL
WlHlTokenIsAdmin(
    _In_ HANDLE Token)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    PSID Admins = NULL;
    PTOKEN_GROUPS Groups;
    DWORD Size = 0, i;
    BOOL Found = FALSE;

    if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &Admins))
        return FALSE;
    GetTokenInformation(Token, TokenGroups, NULL, 0, &Size);
    Groups = LocalAlloc(LMEM_FIXED, Size);
    if (Groups && GetTokenInformation(Token, TokenGroups, Groups, Size, &Size))
    {
        for (i = 0; i < Groups->GroupCount && !Found; i++)
            Found = EqualSid(Groups->Groups[i].Sid, Admins);
    }
    if (Groups)
        LocalFree(Groups);
    FreeSid(Admins);
    return Found;
}

static DWORD WlHlPolicyDword(_In_ PCWSTR Name, _In_ DWORD Default);

/* 8.10 d: in safe mode, SafeModeBlockNonAdmins keeps non-administrators out, as on Windows. */
static BOOL
WlHlSafeModeBlocksNonAdmins(VOID)
{
    DWORD Option = 0, Size = sizeof(Option);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Option",
                     L"OptionValue", RRF_RT_REG_DWORD, NULL, &Option, &Size) != ERROR_SUCCESS || Option == 0)
        return FALSE;
    return WlHlPolicyDword(L"SafeModeBlockNonAdmins", 0) != 0;
}

/* Interactive or unlock logon through MSV1_0, with the logon SID groups msgina built. */
static NTSTATUS
WlHlLsaLogon(
    _In_ const CREDSER_LOGON *Logon,
    _In_ BOOL Unlock,
    _Out_ PHANDLE Token,
    _Out_ PLUID LogonId,
    _Out_ PNTSTATUS SubStatus)
{
    PMSV1_0_INTERACTIVE_LOGON Msv = NULL;
    PTOKEN_GROUPS Groups;
    ULONG MsvSize = 0, ProfileSize = 0;
    PVOID Profile = NULL;
    LSA_STRING Origin;
    TOKEN_SOURCE Source;
    QUOTA_LIMITS Quotas;
    NTSTATUS Status;

    *Token = NULL;
    *SubStatus = STATUS_SUCCESS;
    Status = CredSerBuildMsv1_0Logon(Logon, &Msv, &MsvSize);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Unlock)
        Msv->MessageType = MsV1_0WorkstationUnlockLogon;    /* same buffer as msgina lsa.c, unlock type */

    RtlInitString(&Origin, "Winlogon");
    CopyMemory(Source.SourceName, "User32  ", TOKEN_SOURCE_LENGTH);
    AllocateLocallyUniqueId(&Source.SourceIdentifier);

    Groups = WlHlBuildLogonGroups();
    Status = Groups ? LsaLogonUser(g_Lsa, &Origin, Interactive, g_MsvPackage, Msv, MsvSize, Groups, &Source,
                                   &Profile, &ProfileSize, LogonId, Token, &Quotas, SubStatus)
                    : (NTSTATUS)STATUS_NO_MEMORY;
    WlHlFreeLogonGroups(Groups);
    if (Profile)
        LsaFreeReturnBuffer(Profile);
    CredSerFree(Msv, MsvSize);
    if (NT_SUCCESS(Status) && !WlHlTokenHasLogonSid(*Token))
        ERR("WL: logon token carries no logon SID; AllowAccessOnSession will refuse it\n");
    return Status;
}

/* msgina lsa.c MyChangePassword: the MSV1_0 change request, built from the captured copy. */
static NTSTATUS
WlHlLsaChangePassword(
    _In_ const CREDSER_CHANGE_PASSWORD *Change)
{
    const UNICODE_STRING *Values[4] = { &Change->DomainName, &Change->AccountName,
                                        &Change->OldPassword, &Change->NewPassword };
    PMSV1_0_CHANGEPASSWORD_REQUEST Request;
    PMSV1_0_CHANGEPASSWORD_RESPONSE Response = NULL;
    UNICODE_STRING *Fields[4];
    ULONG Size, ResponseSize = 0, i;
    NTSTATUS Status, Protocol = STATUS_SUCCESS;
    PWSTR Ptr;

    Size = sizeof(*Request);
    for (i = 0; i < ARRAYSIZE(Values); i++)
        Size += Values[i]->Length + sizeof(WCHAR);
    Request = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, Size);
    if (Request == NULL)
        return STATUS_NO_MEMORY;

    Request->MessageType = MsV1_0ChangePassword;
    Request->Impersonating = FALSE;
    Fields[0] = &Request->DomainName;
    Fields[1] = &Request->AccountName;
    Fields[2] = &Request->OldPassword;
    Fields[3] = &Request->NewPassword;
    Ptr = (PWSTR)(Request + 1);
    for (i = 0; i < ARRAYSIZE(Values); i++)
    {
        Fields[i]->Length = Values[i]->Length;
        Fields[i]->MaximumLength = Values[i]->Length + sizeof(WCHAR);
        Fields[i]->Buffer = Ptr;
        CopyMemory(Ptr, Values[i]->Buffer, Values[i]->Length);
        Ptr += Values[i]->Length / sizeof(WCHAR) + 1;
    }

    Status = LsaCallAuthenticationPackage(g_Lsa, g_MsvPackage, Request, Size,
                                          (PVOID *)&Response, &ResponseSize, &Protocol);
    SecureZeroMemory(Request, Size);
    LocalFree(Request);
    if (Response)
        LsaFreeReturnBuffer(Response);
    return NT_SUCCESS(Status) ? Protocol : Status;
}

/* ---- commands and events to the SAS thread --------------------------------------- */

static DWORD
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
    DWORD Sequence = 0;

    ZeroMemory(&Command, sizeof(Command));
    Command.Type = Type;
    Command.Scenario = Scenario;
    Command.Flags = Flags;
    Command.Status = Status;
    Command.SubStatus = SubStatus;
    if (User) StringCchCopyW(Command.User, ARRAYSIZE(Command.User), User);
    if (Text) StringCchCopyW(Command.Text, ARRAYSIZE(Command.Text), Text);

    /* RPC threads post results while the SAS thread may be swapping hosts. */
    EnterCriticalSection(&g_LaunchLock);
    if (g_Launch != NULL)
        Sequence = WlHostPost(g_Launch, &Command);
    LeaveCriticalSection(&g_LaunchLock);
    SecureZeroMemory(&Command, sizeof(Command));
    return Sequence;
}

/* Waits until the host has painted the screen of command Sequence; bounded, never a hang. */
static
BOOL
WlHlWaitShown(
    _In_ DWORD Sequence)
{
    BOOL Shown = FALSE;

    if (Sequence != 0 && g_Launch != NULL)
        Shown = WlHostWaitShown(g_Launch, Sequence, WLHL_SHOWN_MS);
    LogonStage(Shown ? "host-shown" : "host-shown-timeout");
    return Shown;
}

static VOID
WlHlPostResource(
    _In_ WLUI_COMMAND_TYPE Type,
    _In_ DWORD Flags,
    _In_ UINT StringId)
{
    WCHAR Text[WLUI_MAX_TEXT];

    if (LoadStringW(hAppInstance, StringId, Text, ARRAYSIZE(Text)) == 0)
        Text[0] = UNICODE_NULL;
    WlHlPost(Type, g_HostScenario, Flags, 0, 0, NULL, Text);
}

/* Events for the SAS thread; it replays them through WlHostOnEvent (sas.c). */
static VOID
WlHlPostEvent(
    _In_ DWORD Event)
{
    if (g_Session != NULL && g_Session->SASWindow != NULL)
        PostMessageW(g_Session->SASWindow, WL_WM_HOST_EVENT, Event, 0);
}

static VOID
WlHlOnHostEnded(
    _In_ PWLHOST_LAUNCH Launch)
{
    UNREFERENCED_PARAMETER(Launch);
    WlHlPostEvent(WLHOST_EVENT_ENDED);
}

/* ---- submits (RPC threads) ------------------------------------------------------- */

/* Parks a logon result for WlHostLoggedOutSas; it is the only consumer. */
static VOID
WlHlFinishLogon(
    _In_ NTSTATUS Status,
    _In_ NTSTATUS SubStatus,
    _In_opt_ HANDLE Token,
    _In_ const LUID *LogonId,
    _In_ const CREDSER_LOGON *Logon,
    _In_ UINT TextId)
{
    EnterCriticalSection(&g_SubmitLock);
    if (!g_Accepting)
    {
        /* The SAS thread stopped waiting meanwhile: nobody would ever consume this logon. */
        LeaveCriticalSection(&g_SubmitLock);
        if (Token)
            CloseHandle(Token);
        return;
    }
    SecureZeroMemory(&g_Submit, sizeof(g_Submit));
    g_Submit.Pending = TRUE;
    g_Submit.Status = Status;
    g_Submit.SubStatus = SubStatus;
    g_Submit.MustChange = WlHlMustChange(Status, SubStatus);
    g_Submit.TextId = TextId;
    if (NT_SUCCESS(Status) || g_Submit.MustChange)
    {
        /* The change page gets the account name back; the password only follows a success. */
        WlHlCopyString(&Logon->UserName, g_Submit.User, ARRAYSIZE(g_Submit.User));
        WlHlCopyString(&Logon->LogonDomainName, g_Submit.Domain, ARRAYSIZE(g_Submit.Domain));
    }
    if (NT_SUCCESS(Status))
    {
        g_Submit.Token = Token;
        g_Submit.LogonId = *LogonId;
        /* The network-provider notification (MPR) still wants the password; it is wiped after use. */
        WlHlCopyString(&Logon->Password, g_Submit.Password, ARRAYSIZE(g_Submit.Password));
    }
    LeaveCriticalSection(&g_SubmitLock);
    SetEvent(g_SubmitEvent);
}

/* An unlock has no waiting thread: the verdict goes to the SAS window as an event. */
static VOID
WlHlFinishUnlock(
    _In_ NTSTATUS Status,
    _In_ NTSTATUS SubStatus,
    _In_opt_ HANDLE Token)
{
    BOOL Same;

    if (WlHlMustChange(Status, SubStatus))
    {
        if (Token)
            CloseHandle(Token);
        WlHlPostEvent(WLHOST_EVENT_MUST_CHANGE);
        return;
    }
    if (!NT_SUCCESS(Status))
    {
        WlHlPost(WluiCmdReportResult, WLHL_CPUS_UNLOCK, 0, Status, SubStatus, NULL, NULL);
        return;
    }

    Same = WlHlSameTokenUser(Token, g_Session->UserToken);
    CloseHandle(Token);                 /* the session keeps its own token; this one only answered */
    if (Same)
    {
        WlHostOnSas();                  /* the tier that unlocked starts fresh next time (leg finding 1) */
        WlHlPostEvent(WLHOST_EVENT_UNLOCK);
        return;
    }
    /* Another account, administrator or not: the forced-logoff confirmation is Phase 4 UI. */
    ERR("WL: unlock refused: credentials belong to another account\n");
    WlHlPostResource(WluiCmdReportResult, 0, IDS_UNLOCKONLYSESSIONUSER);
}

static DWORD
WlHlHandleLogon(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ DWORD Size,
    _In_ BOOL Unlock)
{
    CREDSER_LOGON Logon;
    HANDLE Token = NULL;
    LUID LogonId;
    NTSTATUS Status, SubStatus = STATUS_SUCCESS;

    UINT TextId = 0;

    if (!NT_SUCCESS(CredSerUnpackLogon(Copy, Size, &Logon)))
        return ERROR_INVALID_PARAMETER;
    Status = WlHlLsaLogon(&Logon, Unlock, &Token, &LogonId, &SubStatus);
    if (!Unlock && NT_SUCCESS(Status) && WlHlSafeModeBlocksNonAdmins() && !WlHlTokenIsAdmin(Token))
    {
        ERR("WL: safe mode: non-administrator logon refused (SafeModeBlockNonAdmins)\n");
        CloseHandle(Token);
        Token = NULL;
        Status = STATUS_ACCOUNT_RESTRICTION;
        TextId = IDS_SAFEMODEADMINSONLY;
    }
    if (Unlock)
        WlHlFinishUnlock(Status, SubStatus, Token);
    else
        WlHlFinishLogon(Status, SubStatus, Token, &LogonId, &Logon, TextId);
    return ERROR_SUCCESS;
}

/* The change a signed-in user asked for may only name that user (msgina fixes the name too). */
static BOOL
WlHlIsSessionAccount(
    _In_ PCUNICODE_STRING Domain,
    _In_ PCUNICODE_STRING Account)
{
    WCHAR Computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD Chars = ARRAYSIZE(Computer);
    UNICODE_STRING Want, Here;
    PCWSTR SessionDomain;
    BOOL TypedLocal, SessionLocal;

    if (g_Session == NULL || g_Session->UserName == NULL)
        return FALSE;
    RtlInitUnicodeString(&Want, g_Session->UserName);
    if (!RtlEqualUnicodeString(Account, &Want, TRUE))
        return FALSE;

    /* "Local" is an empty domain, a lone dot, or this computer's name, on either side. */
    if (!GetComputerNameW(Computer, &Chars))
        Computer[0] = UNICODE_NULL;
    RtlInitUnicodeString(&Here, Computer);
    TypedLocal = Domain->Length == 0 ||
                 (Domain->Length == sizeof(WCHAR) && Domain->Buffer[0] == L'.') ||
                 (Here.Length != 0 && RtlEqualUnicodeString(Domain, &Here, TRUE));
    SessionDomain = g_Session->Domain ? g_Session->Domain : L"";
    RtlInitUnicodeString(&Want, SessionDomain);
    SessionLocal = Want.Length == 0 || !wcscmp(SessionDomain, L".") ||
                   (Here.Length != 0 && RtlEqualUnicodeString(&Want, &Here, TRUE));
    if (TypedLocal || SessionLocal)
        return TypedLocal && SessionLocal;
    return RtlEqualUnicodeString(Domain, &Want, TRUE);
}

/* A change the host was asked for; on success the interrupted logon or unlock continues
 * with the new password, as Windows does. */
static DWORD
WlHlHandleChange(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ DWORD Size,
    _In_ DWORD Resume)
{
    CREDSER_CHANGE_PASSWORD Change;
    CREDSER_LOGON Logon;
    HANDLE Token = NULL;
    LUID LogonId;
    NTSTATUS Status, SubStatus = STATUS_SUCCESS;

    if (!NT_SUCCESS(CredSerUnpackChangePassword(Copy, Size, &Change)))
        return ERROR_INVALID_PARAMETER;
    if (Resume == WLHL_CHANGE_SIGNED_IN && !WlHlIsSessionAccount(&Change.DomainName, &Change.AccountName))
    {
        ERR("WL: a signed-in change named another account; refused\n");
        WlHlPost(WluiCmdReportResult, WLHL_CPUS_CHANGE_PASSWORD, 0, STATUS_ACCESS_DENIED, 0, NULL, NULL);
        return ERROR_SUCCESS;
    }
    /* LSA verifies the old password; nothing is checked here. */
    Status = WlHlLsaChangePassword(&Change);
    if (!NT_SUCCESS(Status))
    {
        WlHlPost(WluiCmdReportResult, WLHL_CPUS_CHANGE_PASSWORD, 0, Status, 0, NULL, NULL);
        return ERROR_SUCCESS;
    }
    if (Resume == WLHL_CHANGE_SIGNED_IN)
    {
        /* The user stays signed in: no new logon, just the result, then back to the desktop. */
        WlHlPost(WluiCmdReportResult, WLHL_CPUS_CHANGE_PASSWORD, 0, STATUS_SUCCESS, 0, NULL, NULL);
        EnterCriticalSection(&g_SubmitLock);
        if (g_OptionsUp)
        {
            g_PendingAction = WLHL_ACTION_CHANGED;
            SetEvent(g_SubmitEvent);
        }
        LeaveCriticalSection(&g_SubmitLock);
        return ERROR_SUCCESS;
    }

    ZeroMemory(&Logon, sizeof(Logon));
    Logon.MessageType = KerbInteractiveLogon;
    Logon.LogonDomainName = Change.DomainName;
    Logon.UserName = Change.AccountName;
    Logon.Password = Change.NewPassword;
    Status = WlHlLsaLogon(&Logon, Resume == WLHL_CPUS_UNLOCK, &Token, &LogonId, &SubStatus);
    if (Resume == WLHL_CPUS_UNLOCK)
        WlHlFinishUnlock(Status, SubStatus, Token);
    else
        WlHlFinishLogon(Status, SubStatus, Token, &LogonId, &Logon, 0);
    return ERROR_SUCCESS;
}

/* The page winlogon asked for is the only one whose submission is taken; the host never picks. */
static BOOL
WlHlScenarioExpected(
    _In_ DWORD Scenario)
{
    LOGON_STATE State = g_Session->LogonState;

    switch (Scenario)
    {
        case WLHL_CPUS_LOGON:
            return g_ChangeFor == 0 && g_Accepting;
        case WLHL_CPUS_UNLOCK:
            return g_ChangeFor == 0 && (State == STATE_LOCKED || State == STATE_LOCKED_SAS);
        case WLHL_CPUS_CHANGE_PASSWORD:
            return g_ChangeFor != 0;
        default:
            return FALSE;
    }
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
    PVOID Copy = NULL;
    DWORD Error, Resume;

    UNREFERENCED_PARAMETER(State);

    if (Scenario != WLHL_CPUS_LOGON && Scenario != WLHL_CPUS_UNLOCK && Scenario != WLHL_CPUS_CHANGE_PASSWORD)
        return ERROR_NOT_SUPPORTED;
    if (!g_Connected)
        return ERROR_INVALID_PARAMETER;
    if (Role == WluiAuthRoleRecovery)
    {
        /* P4: the recovery dialog has no LSA handle and only ever speaks for the password
         * provider; it sends package 0 and gets MSV1_0, nothing else is accepted from it. */
        if (AuthenticationPackage != WLUI_PACKAGE_SERVER_DEFAULT || !IsEqualGUID(ProviderClsid, &WlHlPasswordProvider))
            return ERROR_ACCESS_DENIED;
        AuthenticationPackage = g_MsvPackage;
    }
    else if (!CredSerIsKnownPackage(&g_Packages, AuthenticationPackage))
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (!WlHlProviderRegistered(ProviderClsid))
        return ERROR_ACCESS_DENIED;
    if (Serialization == NULL || cbSerialization == 0 || cbSerialization > CREDSER_MAX_BLOB_SIZE)
        return ERROR_INVALID_PARAMETER;

    /* One submit at a time, and only for the page winlogon is showing (R2). */
    EnterCriticalSection(&g_SubmitLock);
    if (g_SubmitBusy || g_Submit.Pending || !WlHlScenarioExpected(Scenario))
    {
        LeaveCriticalSection(&g_SubmitLock);
        return ERROR_BUSY;
    }
    g_SubmitBusy = TRUE;
    Resume = g_ChangeFor;
    LeaveCriticalSection(&g_SubmitLock);

    /* One private copy; every later step reads only the copy. */
    if (!NT_SUCCESS(CredSerCapture(Serialization, cbSerialization, &Copy)))
        Error = ERROR_INVALID_PARAMETER;
    else if (Scenario == WLHL_CPUS_CHANGE_PASSWORD)
        Error = WlHlHandleChange(Copy, cbSerialization, Resume);
    else
        Error = WlHlHandleLogon(Copy, cbSerialization, Scenario == WLHL_CPUS_UNLOCK);
    CredSerFree(Copy, cbSerialization);

    EnterCriticalSection(&g_SubmitLock);
    g_SubmitBusy = FALSE;
    LeaveCriticalSection(&g_SubmitLock);
    return Error;
}

static BOOL WlHlDisableCad(VOID);
static VOID WlHlCloseSwitchHandles(VOID);

/* Policy values live under Policies\System on Windows; the Winlogon key is the older place. */
static DWORD
WlHlPolicyDword(
    _In_ PCWSTR Name,
    _In_ DWORD Default)
{
    DWORD Value, Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_POLICY_KEY, Name, RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS)
        return Value;
    Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, Name, RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS)
        return Value;
    return Default;
}

/* msgina.c read it from the Winlogon key; workstation default TRUE. */
static BOOL
WlHlShutdownWithoutLogon(VOID)
{
    return WlHlPolicyDword(L"ShutdownWithoutLogon", 1) != 0;
}

/* A legal notice is configured (Policies\System legalnoticetext, or the Winlogon key). */
static BOOL
WlHlHasLegalNotice(VOID)
{
    WCHAR Text[4];
    DWORD Size = sizeof(Text);
    LONG Error;

    Error = RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_POLICY_KEY, L"legalnoticetext", RRF_RT_REG_SZ, NULL, Text, &Size);
    if (Error != ERROR_SUCCESS && Error != ERROR_MORE_DATA)
    {
        Size = sizeof(Text);
        Error = RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, L"LegalNoticeText", RRF_RT_REG_SZ, NULL, Text, &Size);
    }
    return (Error == ERROR_SUCCESS || Error == ERROR_MORE_DATA) && Size > sizeof(WCHAR);
}

/* ---- automatic sign-in (AutoAdminLogon), as msgina GetRegistrySettings + LogonUiSignIn ---- */

static BOOL g_AutoLogonTried;           /* an automatic sign-in was considered this boot */
static BOOL g_AutoLogonAgain;           /* ForceAutoLogon: the next logged-off state may sign in again */

/* AutoAdminLogon and its companions are REG_SZ numbers on Windows; a REG_DWORD is accepted too. */
static DWORD
WlHlWinlogonNumber(
    _In_ PCWSTR Name,
    _In_ DWORD Default)
{
    WCHAR Text[16];
    DWORD Type, Size = sizeof(Text) - sizeof(WCHAR), Value = Default;
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, 0, KEY_QUERY_VALUE, &Key) != ERROR_SUCCESS)
        return Default;
    if (RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)Text, &Size) == ERROR_SUCCESS)
    {
        if (Type == REG_DWORD && Size == sizeof(DWORD))
            Value = *(PDWORD)Text;
        else if (Type == REG_SZ)
        {
            Text[Size / sizeof(WCHAR)] = UNICODE_NULL;
            Value = wcstoul(Text, NULL, 10);
        }
    }
    RegCloseKey(Key);
    return Value;
}

static VOID
WlHlWinlogonString(
    _In_ PCWSTR Name,
    _Out_writes_(Chars) PWSTR Text,
    _In_ DWORD Chars)
{
    DWORD Size = Chars * sizeof(WCHAR);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, Name, RRF_RT_REG_SZ, NULL, Text, &Size) != ERROR_SUCCESS)
        Text[0] = UNICODE_NULL;
}

/* The protected copy (LSA secret DefaultPassword) wins over the registry value. */
static BOOL
WlHlLsaDefaultPassword(
    _Out_writes_(Chars) PWSTR Password,
    _In_ DWORD Chars)
{
    LSA_OBJECT_ATTRIBUTES Attributes;
    LSA_HANDLE Policy;
    LSA_UNICODE_STRING Name, *Secret = NULL;
    NTSTATUS Status;

    Password[0] = UNICODE_NULL;
    ZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Length = sizeof(Attributes);
    if (!NT_SUCCESS(LsaOpenPolicy(NULL, &Attributes, POLICY_GET_PRIVATE_INFORMATION, &Policy)))
        return FALSE;
    RtlInitUnicodeString(&Name, L"DefaultPassword");
    Status = LsaRetrievePrivateData(Policy, &Name, &Secret);
    LsaClose(Policy);
    if (!NT_SUCCESS(Status) || Secret == NULL)
        return FALSE;
    if (Secret->Length / sizeof(WCHAR) >= Chars)
        Status = STATUS_BUFFER_TOO_SMALL;
    else if (Secret->Length != 0)
        CopyMemory(Password, Secret->Buffer, Secret->Length);
    if (NT_SUCCESS(Status))
        Password[Secret->Length / sizeof(WCHAR)] = UNICODE_NULL;
    if (Secret->Buffer != NULL)
        SecureZeroMemory(Secret->Buffer, Secret->Length);
    LsaFreeMemory(Secret);
    return NT_SUCCESS(Status);
}

/* Windows turns automatic sign-in off with AutoAdminLogon "0"; a spent AutoLogonCount also drops the password. */
static VOID
WlHlDisableAutoLogon(
    _In_ BOOL Spent)
{
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, 0, KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
        return;
    RegSetValueExW(Key, L"AutoAdminLogon", 0, REG_SZ, (const BYTE *)L"0", sizeof(L"0"));
    if (Spent)
    {
        RegDeleteValueW(Key, L"DefaultPassword");
        RegDeleteValueW(Key, L"AutoLogonCount");
    }
    RegCloseKey(Key);
}

static BOOL
WlHlAutoLogonWanted(VOID)
{
    if (g_AutoLogonTried && !g_AutoLogonAgain)
        return FALSE;
    return WlHlWinlogonNumber(L"AutoAdminLogon", 0) != 0 && !WlHlHasLegalNotice();
}

/* Signs in DefaultUserName once; the result goes to WlHostLoggedOutSas like a host submit. Never on unlock. */
static BOOL
WlHlTryAutoLogon(VOID)
{
    CREDSER_LOGON Logon;
    WCHAR User[256], Domain[256], Password[256];
    HKEY Key;
    DWORD Left, Chars;
    HANDLE Token = NULL;
    LUID LogonId = { 0, 0 };
    NTSTATUS Status, SubStatus = STATUS_SUCCESS;

    if (!WlHlAutoLogonWanted())
        return FALSE;
    g_AutoLogonTried = TRUE;
    g_AutoLogonAgain = FALSE;
    if (WlHlWinlogonNumber(L"IgnoreShiftOverride", 0) == 0 && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
    {
        ERR("WL: automatic sign-in skipped: Shift is held\n");
        return FALSE;
    }
    Left = WlHlWinlogonNumber(L"AutoLogonCount", MAXDWORD);
    if (Left == 0)
    {
        WlHlDisableAutoLogon(TRUE);
        return FALSE;
    }
    WlHlWinlogonString(L"DefaultUserName", User, ARRAYSIZE(User));
    if (User[0] == UNICODE_NULL)
        return FALSE;
    WlHlWinlogonString(L"DefaultDomainName", Domain, ARRAYSIZE(Domain));
    Chars = ARRAYSIZE(Domain);
    if (Domain[0] == UNICODE_NULL && !GetComputerNameW(Domain, &Chars))
        Domain[0] = UNICODE_NULL;
    if (!WlHlLsaDefaultPassword(Password, ARRAYSIZE(Password)))
        WlHlWinlogonString(L"DefaultPassword", Password, ARRAYSIZE(Password));

    ZeroMemory(&Logon, sizeof(Logon));
    Logon.MessageType = KerbInteractiveLogon;
    RtlInitUnicodeString(&Logon.LogonDomainName, Domain);
    RtlInitUnicodeString(&Logon.UserName, User);
    RtlInitUnicodeString(&Logon.Password, Password);
    Status = WlHlLsaLogon(&Logon, FALSE, &Token, &LogonId, &SubStatus);
    if (NT_SUCCESS(Status) && WlHlSafeModeBlocksNonAdmins() && !WlHlTokenIsAdmin(Token))
    {
        CloseHandle(Token);
        Token = NULL;
        Status = STATUS_ACCOUNT_RESTRICTION;
    }
    if (NT_SUCCESS(Status))
    {
        if (Left != MAXDWORD && --Left == 0)
            WlHlDisableAutoLogon(TRUE);
        else if (Left != MAXDWORD &&
                 RegOpenKeyExW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, 0, KEY_SET_VALUE, &Key) == ERROR_SUCCESS)
        {
            RegSetValueExW(Key, L"AutoLogonCount", 0, REG_DWORD, (const BYTE *)&Left, sizeof(Left));
            RegCloseKey(Key);
        }
        g_AutoLogonAgain = WlHlWinlogonNumber(L"ForceAutoLogon", 0) != 0;
        WlHlFinishLogon(Status, SubStatus, Token, &LogonId, &Logon, 0);
    }
    else
    {
        ERR("WL: automatic sign-in failed (0x%08lx); AutoAdminLogon turned off\n", Status);
        WlHlDisableAutoLogon(FALSE);
    }
    SecureZeroMemory(Password, sizeof(Password));
    SecureZeroMemory(&Logon, sizeof(Logon));
    return NT_SUCCESS(Status);
}

/* Flags for RequestCredentials that describe policy to the host; winlogon stays the gate. */
static DWORD
WlHlPolicyFlags(VOID)
{
    DWORD Flags = 0;
    if (WlHlDisableCad())
        Flags |= WLUI_FLAG_DISABLE_CAD;
    if (!WlHlShutdownWithoutLogon())
        Flags |= WLUI_FLAG_NO_POWER;
    if (WlHlHasLegalNotice())
        Flags |= WLUI_FLAG_LEGAL_NOTICE;
    return Flags;
}

static DWORD
WlHlActionHandler(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ WLUI_AUTH_ROLE Role,
    _In_ DWORD Action)
{
    LOGON_STATE LogonState;

    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(Role);

    if (Action < WluiActionShutdown || Action > WluiActionSleep)
        return ERROR_INVALID_PARAMETER;

    /* The Ctrl+Alt+Del options: only while that page is up, and only what its policy allowed. */
    EnterCriticalSection(&g_SubmitLock);
    if (g_OptionsUp)
    {
        DWORD Denied = 0;

        if (Action == WluiActionLock)
            Denied = g_OptionsFlags & WLUI_FLAG_NO_LOCK;
        else if (Action == WluiActionLogoff)
            Denied = g_OptionsFlags & WLUI_FLAG_NO_LOGOFF;
        else if (Action == WluiActionChangePassword)
            Denied = g_OptionsFlags & WLUI_FLAG_NO_CHANGE_PASSWORD;
        else if (Action == WluiActionTaskManager)
            Denied = g_OptionsFlags & WLUI_FLAG_NO_TASK_MANAGER;
        else if (Action == WluiActionShutdown || Action == WluiActionRestart || Action == WluiActionEmergencyRestart ||
                 Action == WluiActionSleep)
            Denied = g_OptionsFlags & WLUI_FLAG_NO_POWER;
        if (Denied)
        {
            LeaveCriticalSection(&g_SubmitLock);
            ERR("WL: option %lu refused by policy\n", Action);
            return ERROR_ACCESS_DENIED;
        }
        g_PendingAction = Action;
        SetEvent(g_SubmitEvent);
        LeaveCriticalSection(&g_SubmitLock);
        return ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_SubmitLock);

    if (Action == WluiActionCancel)
        return ERROR_SUCCESS;
    if (Action != WluiActionShutdown && Action != WluiActionRestart && Action != WluiActionSleep)
        return ERROR_ACCESS_DENIED;     /* an options action with no options page up */

    /* Nobody is signed in on the notice, the sign-in page or the locked station: policy decides. */
    LogonState = g_Session->LogonState;
    if ((LogonState == STATE_LOGGED_OFF || LogonState == STATE_LOGGED_OFF_SAS ||
         LogonState == STATE_LOCKED || LogonState == STATE_LOCKED_SAS) &&
        !WlHlShutdownWithoutLogon())
    {
        ERR("WL: power action %lu refused: ShutdownWithoutLogon is 0\n", Action);
        return ERROR_ACCESS_DENIED;
    }

    EnterCriticalSection(&g_SubmitLock);
    if (g_Accepting)
    {
        /* WlHostLoggedOutSas is waiting and returns the action to DispatchSAS itself. */
        g_PendingAction = Action;
        SetEvent(g_SubmitEvent);
    }
    else
    {
        WlHlPostEvent(Action == WluiActionShutdown ? WLHOST_EVENT_SHUTDOWN :
                      Action == WluiActionSleep ? WLHOST_EVENT_SLEEP : WLHOST_EVENT_RESTART);
    }
    LeaveCriticalSection(&g_SubmitLock);
    return ERROR_SUCCESS;
}

/* ---- the live host (SAS thread) ---------------------------------------------------- */

/* Joined to a domain; when the machine cannot tell, it answers yes, so Ctrl+Alt+Del stays asked. */
static BOOL
WlHlDomainJoined(VOID)
{
    LSA_OBJECT_ATTRIBUTES Attributes;
    LSA_HANDLE Policy;
    PPOLICY_PRIMARY_DOMAIN_INFO Info = NULL;
    BOOL Joined = TRUE;

    ZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Length = sizeof(Attributes);
    if (!NT_SUCCESS(LsaOpenPolicy(NULL, &Attributes, POLICY_VIEW_LOCAL_INFORMATION, &Policy)))
        return TRUE;
    if (NT_SUCCESS(LsaQueryInformationPolicy(Policy, PolicyPrimaryDomainInformation, (PVOID *)&Info)) && Info)
    {
        Joined = (Info->Sid != NULL);
        LsaFreeMemory(Info);
    }
    LsaClose(Policy);
    return Joined;
}

/*
 * Whether Ctrl+Alt+Del is optional, as Windows decides it: the policy value first, then the
 * Winlogon value, else by membership: a workgroup PC does not ask for it, a domain member does.
 */
static BOOL
WlHlDisableCad(VOID)
{
    DWORD Value = 0, Size = sizeof(Value);

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"DisableCAD", RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS)
        return Value != 0;
    Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, L"DisableCAD",
                     RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS)
        return Value != 0;
    return !WlHlDomainJoined();
}

/* P2: how long a lock or logoff waits for the host's first frame before switching anyway. */
static DWORD
WlHlLockReadyMs(VOID)
{
    DWORD Value = 0, Size = sizeof(Value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WLHL_WINLOGON_KEY, L"LockReadyTimeoutMs",
                     RRF_RT_REG_DWORD, NULL, &Value, &Size) != ERROR_SUCCESS || Value > 60000)
        Value = WLHL_LOCK_READY_MS;
    return Value;
}

static BOOL
WlHlHostLive(VOID)
{
    return g_Launch != NULL && !WlHostHasEnded(g_Launch, NULL, NULL);
}

static VOID
WlHlStopHost(VOID)
{
    PWLHOST_LAUNCH Old;

    EnterCriticalSection(&g_LaunchLock);
    Old = g_Launch;
    g_Launch = NULL;
    g_HostScenario = 0;
    LeaveCriticalSection(&g_LaunchLock);
    if (Old != NULL)
        WlHostStop(Old);
}

/* ---- N4: the terminal dialog winlogon draws itself (8.9 P3, 8.10 c, R1b) ---------------- */

static INT_PTR CALLBACK
WlHlTerminalProc(
    _In_ HWND Dialog,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Text[512];
            UINT Id = (lParam == WlHostTerminalMissing) ? IDS_LOGONUI_TERMINAL_MISSING :
                      (lParam == WlHostTerminalPolicy) ? IDS_LOGONUI_TERMINAL_POLICY : IDS_LOGONUI_TERMINAL_LEVELS;
            LOGON_STATE State = g_Session ? g_Session->LogonState : STATE_LOGGED_OFF;
            BOOL SignedOut = (State == STATE_LOGGED_OFF || State == STATE_LOGGED_OFF_SAS ||
                              State == STATE_LOCKED || State == STATE_LOCKED_SAS);
            BOOL Power = !SignedOut || WlHlShutdownWithoutLogon();     /* same gate as the host's flyout */

            if (LoadStringW(hAppInstance, Id, Text, ARRAYSIZE(Text)) == 0)
                Text[0] = UNICODE_NULL;
            SetDlgItemTextW(Dialog, IDC_LUT_TEXT, Text);
            EnableWindow(GetDlgItem(Dialog, IDC_LUT_RESTART), Power);
            EnableWindow(GetDlgItem(Dialog, IDC_LUT_SHUTDOWN), Power);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_LUT_RESTART || LOWORD(wParam) == IDC_LUT_SHUTDOWN)
            {
                WlHlPostEvent(LOWORD(wParam) == IDC_LUT_RESTART ? WLHOST_EVENT_RESTART : WLHOST_EVENT_SHUTDOWN);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
                return TRUE;            /* nothing dismisses it but a launch or a power action */
            break;
        case WM_LUT_CLOSE:
            DestroyWindow(Dialog);
            return TRUE;
        case WM_CLOSE:
            return TRUE;
        case WM_DESTROY:
            g_TerminalDialog = NULL;
            PostQuitMessage(0);
            return TRUE;
    }
    return FALSE;
}

/* The dialog lives on its own thread with its own loop: winlogon's main loop is filtered
 * to the SAS window and has no IsDialogMessage, so a dialog on that thread would be dead. */
static DWORD WINAPI
WlHlTerminalThread(
    _In_ LPVOID Parameter)
{
    WLHOST_TERMINAL Reason = (WLHOST_TERMINAL)(ULONG_PTR)Parameter;
    HWND Dialog = NULL;
    MSG Msg;

    if (g_Session == NULL || !SetThreadDesktop(g_Session->WinlogonDesktop))
        ERR("WL: terminal dialog thread cannot use the Winlogon desktop (%lu)\n", GetLastError());
    else
        Dialog = CreateDialogParamW(hAppInstance, MAKEINTRESOURCEW(IDD_LOGONUITERMINAL), NULL,
                                    WlHlTerminalProc, (LPARAM)Reason);
    g_TerminalDialog = Dialog;
    SetEvent(g_TerminalShown);
    if (Dialog == NULL)
    {
        ERR("WL: cannot create the terminal dialog (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(Dialog, SW_SHOW);
    SetForegroundWindow(Dialog);
    SetFocus(GetDlgItem(Dialog, IDC_LUT_RESTART));
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Dialog, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }
    return 0;
}

static BOOL
WlHlTerminalUp(VOID)
{
    return g_TerminalThread != NULL && WaitForSingleObject(g_TerminalThread, 0) == WAIT_TIMEOUT &&
           g_TerminalDialog != NULL;
}

/* Joins a dialog thread that is over (or asks a live one to leave first). */
static VOID
WlHlCloseTerminal(VOID)
{
    if (g_TerminalThread == NULL)
        return;
    if (g_TerminalDialog != NULL)
        PostMessageW(g_TerminalDialog, WM_LUT_CLOSE, 0, 0);
    if (WaitForSingleObject(g_TerminalThread, 5000) != WAIT_OBJECT_0)
        ERR("WL: terminal dialog thread did not leave\n");
    CloseHandle(g_TerminalThread);
    g_TerminalThread = NULL;
    g_TerminalDialog = NULL;
}

/* Shown by the SAS thread (not blocked by it); the next launch that succeeds closes it. */
static VOID
WlHlShowTerminal(
    _In_ WLHOST_TERMINAL Reason)
{
    if (WlHlTerminalUp())
        return;
    WlHlCloseTerminal();                /* a thread that already ended on its own */
    if (g_TerminalShown == NULL)
        g_TerminalShown = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_TerminalShown == NULL)
        return;
    ResetEvent(g_TerminalShown);
    g_TerminalThread = CreateThread(NULL, 0, WlHlTerminalThread, (LPVOID)(ULONG_PTR)Reason, 0, NULL);
    if (g_TerminalThread == NULL)
    {
        ERR("WL: cannot start the terminal dialog thread (%lu)\n", GetLastError());
        return;
    }
    WaitForSingleObject(g_TerminalShown, 5000);
    WlHostLog(EVENTLOG_ERROR_TYPE, Reason == WlHostTerminalPolicy ? L"logon UI terminal dialog shown (policy)" :
              Reason == WlHostTerminalMissing ? L"logon UI terminal dialog shown (images missing)" :
                                                L"logon UI terminal dialog shown (levels spent)");
}

/* The change page names the account whose password expired: the typed logon, or the
 * session's user when the change was forced at an unlock. */
static VOID
WlHlChangePrefill(
    _Out_writes_(Chars) PWCHAR Prefill,
    _In_ ULONG Chars)
{
    PCWSTR Domain = NULL, User = NULL;

    EnterCriticalSection(&g_SubmitLock);
    if (g_ChangeFor == WLHL_CPUS_LOGON)
    {
        Domain = g_Submit.Domain;
        User = g_Submit.User;
    }
    else if (g_Session != NULL)
    {
        Domain = g_Session->Domain;
        User = g_Session->UserName;
    }
    if (User == NULL || User[0] == UNICODE_NULL)
        Prefill[0] = UNICODE_NULL;
    else if (Domain != NULL && Domain[0] != UNICODE_NULL)
        StringCchPrintfW(Prefill, Chars, L"%s\\%s", Domain, User);
    else
        StringCchCopyW(Prefill, Chars, User);
    LeaveCriticalSection(&g_SubmitLock);
}

/* The unlock page names the locked session's account, so the host lists only that tile. */
static VOID
WlHlUnlockPrefill(
    _Out_writes_(Chars) PWCHAR Prefill,
    _In_ ULONG Chars)
{
    PCWSTR Domain = g_Session ? g_Session->Domain : NULL;
    PCWSTR User = g_Session ? g_Session->UserName : NULL;

    if (User == NULL || User[0] == UNICODE_NULL)
        Prefill[0] = UNICODE_NULL;
    else if (Domain != NULL && Domain[0] != UNICODE_NULL)
        StringCchPrintfW(Prefill, Chars, L"%s\\%s", Domain, User);
    else
        StringCchCopyW(Prefill, Chars, User);
}

/* Brings up a host for Scenario (0: status only). A live host is kept when it already shows
 * Scenario, or when it was status-only and the logon page is wanted (the host builds its
 * providers at the first RequestCredentials); any other change means a fresh host. */
static BOOL
WlHlEnsureHost(
    _In_ DWORD Scenario,
    _In_ DWORD Flags,
    _In_ DWORD ReadyMs)
{
    PWLHOST_LAUNCH Launch;
    WLHOST_TERMINAL Reason = WlHostTerminalNone;
    BOOL Terminal = FALSE;
    WCHAR Prefill[WLUI_MAX_TEXT];

    if (!g_HostLogonReady)
    {
        /* An ordering mistake must trace, not hang on a zeroed lock (VM leg B.1.1). */
        ERR("WL: host requested before WlHostLogonInit (scenario %lu)\n", Scenario);
        return FALSE;
    }

    EnterCriticalSection(&g_SubmitLock);
    if (Scenario != WLHL_CPUS_CHANGE_PASSWORD)
        g_ChangeFor = 0;
    LeaveCriticalSection(&g_SubmitLock);
    Prefill[0] = UNICODE_NULL;
    if (Scenario == WLHL_CPUS_CHANGE_PASSWORD)
        WlHlChangePrefill(Prefill, ARRAYSIZE(Prefill));
    else if (Scenario == WLHL_CPUS_UNLOCK)
        WlHlUnlockPrefill(Prefill, ARRAYSIZE(Prefill));

    if (WlHlHostLive())
    {
        if (Scenario == 0 || g_HostScenario == Scenario)
            return TRUE;
        if (g_HostScenario == 0 && Scenario == WLHL_CPUS_LOGON)
        {
            g_HostScenario = Scenario;
            WlHlPost(WluiCmdRequestCredentials, Scenario, Flags, 0, 0, NULL, NULL);
            return TRUE;
        }
    }
    WlHlStopHost();

    Launch = WlHostStart(Scenario ? Scenario : WLHL_CPUS_LOGON, &Terminal, &Reason);
    if (Launch == NULL)
    {
        if (Terminal)
            WlHlShowTerminal(Reason);
        return FALSE;
    }
    WlHlCloseTerminal();
    EnterCriticalSection(&g_LaunchLock);
    g_Launch = Launch;
    g_HostScenario = Scenario;
    LeaveCriticalSection(&g_LaunchLock);
    LogonStage("host-launched");

    if (WlHostWaitReady(Launch, ReadyMs))
    {
        LogonStage("host-ready");
    }
    else
    {
        LogonStage("host-not-ready");
        WARN("WL: logon UI not ready after %lu ms, continuing anyway\n", ReadyMs);
    }
    if (Scenario != 0)
        WlHlPost(WluiCmdRequestCredentials, Scenario, Flags, 0, 0, Prefill[0] ? Prefill : NULL, NULL);
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

/* Called right after the selector is read, before any status message: the boot status page
 * ("Orosi is starting up") goes through WlHostStatus and its locks long before the SAS window
 * exists. Entering an uninitialised critical section there stalled winlogon (VM leg B.1.1). */
VOID
WlHostLogonInit(
    _In_ PWLSESSION Session)
{
    g_Session = Session;
    InitializeCriticalSection(&g_SubmitLock);
    InitializeCriticalSection(&g_LaunchLock);
    g_SubmitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_HostLogonReady = TRUE;
}

/* Called once the SAS window exists: events and handlers need it, the locks do not. */
VOID
WlHostAttach(
    _In_ PWLSESSION Session)
{
    /* g_Session was set by WlHostLogonInit; only the handlers wait for the SAS window. */
    UNREFERENCED_PARAMETER(Session);
    WluiSetSubmitHandler(WlHlSubmitHandler);
    WluiSetActionHandler(WlHlActionHandler);
    WlHostSetEndedCallback(WlHlOnHostEnded);
}

/* ---- the logged-out SAS ------------------------------------------------------- */

/* With CAD disabled the credential page is the notice: enter the logged-out SAS by ourselves.
 * With CAD enabled the host shows the Ctrl+Alt+Delete notice until the user's SAS. */
VOID
WlHostDisplaySasNotice(
    _In_ PWLSESSION Session)
{
    if (!WlHlDisableCad() && !WlHlAutoLogonWanted())
    {
        /* G7: the Ctrl+Alt+Del notice is the host's clock curtain; a page host gives way to a status-only one. */
        if (WlHlHostLive() && g_HostScenario != 0)
            WlHlStopHost();
        if (!WlHlHostLive() && !WlHlEnsureHost(0, 0, WlHlLockReadyMs()))
        {
            ERR("WL: no logon UI for the Ctrl+Alt+Delete notice; waiting for a user SAS\n");
            return;
        }
        WlHlPostResource(WluiCmdDisplayStatus, WLUI_FLAG_CURTAIN, IDS_PRESSCTRLALTDELETE);
        return;
    }
    if (WlHostIsTerminal() || g_LsaBroken || WlHlTerminalUp())
    {
        /* N4, policy or no LSA: posting again would spin; the terminal dialog stands and a user
         * SAS is the only way on. */
        ERR("WL: logon UI cannot come up (%s); waiting for a user SAS\n",
            g_LsaBroken ? "no LSA" : WlHlTerminalUp() ? "terminal dialog" : "terminal state");
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
    DWORD CadFlag = WlHlPolicyFlags();

    Session->HostAutoSas = FALSE;
    g_LsaBroken = !WlHlConnectLsa() || g_SubmitEvent == NULL;
    if (g_LsaBroken)
        return WLX_SAS_ACTION_NONE;

    /* Only a SAS the user typed resets the levels; the self-posted one must not (R3). */
    if (UserSas)
        WlHostOnSas();

    /* A self-posted SAS may be an automatic sign-in; with CAD required only a real one raises the page. */
    WlHlSetAccepting(TRUE);
    if (!UserSas && !WlHlTryAutoLogon() && !WlHlDisableCad())
    {
        WlHlSetAccepting(FALSE);
        return WLX_SAS_ACTION_NONE;
    }
    if (!WlHlEnsureHost(WLHL_CPUS_LOGON, CadFlag, WLHL_READY_MS))
    {
        WlHlSetAccepting(FALSE);
        return WLX_SAS_ACTION_NONE;
    }

    for (;;)
    {
        DWORD Which;

        Waits[0] = g_SubmitEvent;
        Waits[1] = WlHostEndedEvent(g_Launch);
        /* Posted messages keep flowing to the SAS window: win32k posts the shutdown notification
         * (WM_LOGONNOTIFY) there and a plain wait starved it (leg finding 2). A second SAS is
         * ignored in STATE_LOGGED_OFF_SAS, but a logoff, shutdown or lock notification runs its
         * whole action nested in here; the state check after the loop is the guard. */
        Which = MsgWaitForMultipleObjects(2, Waits, FALSE, INFINITE, QS_ALLINPUT);
        if (Which == WAIT_OBJECT_0 + 2)
        {
            MSG Msg;
            BOOL Quit = FALSE;
            while (!Quit && PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            {
                if (Msg.message == WM_QUIT)
                {
                    PostQuitMessage((int)Msg.wParam);   /* belongs to winlogon's main loop */
                    Quit = TRUE;
                    break;
                }
                TranslateMessage(&Msg);
                DispatchMessageW(&Msg);
            }
            if (Quit || Session->LogonState != STATE_LOGGED_OFF_SAS || g_Launch == NULL)
            {
                /* The nested action ended the SAS, or the host is gone: this wait is over. */
                WlHlSetAccepting(FALSE);
                return WLX_SAS_ACTION_NONE;
            }
            continue;
        }

        if (Which == WAIT_OBJECT_0 + 1)
        {
            /* The host died or hung; the level machine already chose the next tier. */
            if (!WlHlEnsureHost(WLHL_CPUS_LOGON, CadFlag, WLHL_READY_MS))
                break;
            continue;
        }

        EnterCriticalSection(&g_SubmitLock);
        if (g_PendingAction != 0)
        {
            DWORD Action = g_PendingAction;
            g_PendingAction = 0;
            LeaveCriticalSection(&g_SubmitLock);
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
            if (Action == WluiActionSleep)
            {
                Result = WLX_SAS_ACTION_SHUTDOWN_SLEEP;
                break;
            }
            continue;
        }
        if (!g_Submit.Pending)
        {
            LeaveCriticalSection(&g_SubmitLock);
            continue;
        }
        g_Submit.Pending = FALSE;

        if (g_Submit.MustChange)
        {
            /* Expired password: the change page takes over and the logon resumes after it. */
            g_ChangeFor = WLHL_CPUS_LOGON;
            LeaveCriticalSection(&g_SubmitLock);
            if (!WlHlEnsureHost(WLHL_CPUS_CHANGE_PASSWORD, WLUI_FLAG_FORCED_CHANGE | CadFlag, WLHL_READY_MS))
                break;
            continue;
        }

        if (!NT_SUCCESS(g_Submit.Status))
        {
            NTSTATUS Status = g_Submit.Status, SubStatus = g_Submit.SubStatus;
            UINT TextId = g_Submit.TextId;
            LeaveCriticalSection(&g_SubmitLock);
            /* The provider owns the wording of its result (STATUS_LOGON_FAILURE and friends),
             * unless winlogon refused it for a reason of its own (safe mode). */
            if (TextId)
                WlHlPostResource(WluiCmdReportResult, 0, TextId);
            else
                WlHlPost(WluiCmdReportResult, g_HostScenario, 0, Status, SubStatus, NULL, NULL);
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
        {
            WCHAR EnvName[MAX_COMPUTERNAME_LENGTH + 1];
            PCWSTR EnvDomain = g_Submit.Domain;
            SIZE_T EnvSize;
            LPWSTR Env;

            Session->Profile->dwType = WLX_PROFILE_TYPE_V2_0;   /* local account: default paths */

            /* The GINA profile carries LOGONSERVER (msgina); host mode sets it the same way. */
            if (EnvDomain[0] == UNICODE_NULL ||
                (EnvDomain[0] == L'.' && EnvDomain[1] == UNICODE_NULL))
            {
                DWORD EnvNameSize = ARRAYSIZE(EnvName);
                if (GetComputerNameW(EnvName, &EnvNameSize))
                    EnvDomain = EnvName;
            }
            if (EnvDomain[0] != UNICODE_NULL)
            {
                EnvSize = (wcslen(L"LOGONSERVER=\\\\") + wcslen(EnvDomain) + 2) * sizeof(WCHAR);
                Env = LocalAlloc(LMEM_FIXED, EnvSize);
                if (Env)
                {
                    StringCchPrintfW(Env, EnvSize / sizeof(WCHAR), L"LOGONSERVER=\\\\%s", EnvDomain);
                    Env[EnvSize / sizeof(WCHAR) - 1] = UNICODE_NULL;
                    Session->Profile->pszEnvironment = Env;
                }
            }
        }
        SecureZeroMemory(g_Submit.Password, sizeof(g_Submit.Password));
        g_Submit.Token = NULL;
        g_ChangeFor = 0;
        LeaveCriticalSection(&g_SubmitLock);

        WlHlPost(WluiCmdDisplayWelcome, g_HostScenario, WLUI_FLAG_SPINNER, 0, 0, Session->MprNotifyInfo.pszUserName, NULL);
        WlHostOnSas();                  /* a completed logon resets the levels like a user SAS does */
        Result = WLX_SAS_ACTION_LOGON;
        break;
    }

    WlHlSetAccepting(FALSE);
    return Result;
}

/* ---- the Ctrl+Alt+Del options of a signed-in user --------------------------------- */

#define WLHL_SCENARIO_OPTIONS       0x100   /* g_HostScenario while the options page is up; not a CPUS_* */
#define WLHL_CHANGED_MS             2500    /* how long the 'password changed' result stays up */

/* A DWORD policy under the signed-in user's HKCU (Software\...\Policies), read as that user. */
static BOOL
WlHlUserPolicy(
    _In_ PWLSESSION Session,
    _In_ PCWSTR SubKey,
    _In_ PCWSTR Name)
{
    HKEY User = NULL;
    DWORD Value = 0, Size = sizeof(Value);
    BOOL Set = FALSE;

    if (Session->UserToken == NULL || !ImpersonateLoggedOnUser(Session->UserToken))
        return FALSE;
    if (RegOpenCurrentUser(KEY_QUERY_VALUE, &User) != ERROR_SUCCESS)
        User = NULL;
    RevertToSelf();
    if (User == NULL)
        return FALSE;
    if (RegGetValueW(User, SubKey, Name, RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS && Value != 0)
        Set = TRUE;
    RegCloseKey(User);
    return Set;
}

/* Restart and Shut down from the options need the user's SeShutdownPrivilege (held, enabled or not). */
static BOOL
WlHlUserMayShutdown(
    _In_ PWLSESSION Session)
{
    BYTE Buffer[2048];
    PTOKEN_PRIVILEGES Privileges = (PTOKEN_PRIVILEGES)Buffer;
    DWORD Size = 0, i;
    LUID Shutdown;

    if (Session->UserToken == NULL || !LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &Shutdown) ||
        !GetTokenInformation(Session->UserToken, TokenPrivileges, Buffer, sizeof(Buffer), &Size))
        return FALSE;
    for (i = 0; i < Privileges->PrivilegeCount; i++)
    {
        if (Privileges->Privileges[i].Luid.LowPart == Shutdown.LowPart &&
            Privileges->Privileges[i].Luid.HighPart == Shutdown.HighPart)
            return TRUE;
    }
    return FALSE;
}

/* What the options page offers, as msgina's LoadOptions decides it (HKLM, then the user's policies). */
static DWORD
WlHlOptionsFlags(
    _In_ PWLSESSION Session)
{
    static const PCWSTR System = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    static const PCWSTR Explorer = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    DWORD Flags = 0;

    /* The same rule as every other lock path (WlIsLockOk): HKLM, then the user's policy. */
    if (!WlHostIsLockOk(Session))
        Flags |= WLUI_FLAG_NO_LOCK;
    if (WlHlUserPolicy(Session, System, L"DisableChangePassword"))
        Flags |= WLUI_FLAG_NO_CHANGE_PASSWORD;
    if (WlHlUserPolicy(Session, System, L"DisableTaskMgr"))
        Flags |= WLUI_FLAG_NO_TASK_MANAGER;
    if (WlHlUserPolicy(Session, Explorer, L"NoLogoff"))
        Flags |= WLUI_FLAG_NO_LOGOFF;
    if (!WlHlUserMayShutdown(Session))
        Flags |= WLUI_FLAG_NO_POWER;
    return Flags;
}

/* Restart now, without notifying anyone: the options page's last resort (msgina Page::Emergency). */
static VOID
WlHlEmergencyRestart(VOID)
{
    BOOLEAN Old;

    ERR("WL: emergency restart requested from the options page\n");
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &Old);
    NtShutdownSystem(ShutdownReboot);
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, Old, FALSE, &Old);
}

/*
 * The SAS of a signed-in user: the host shows the options and the choice comes back as an
 * action. Returns the WLX_SAS_ACTION_* for DispatchSAS; the host is gone when this returns.
 */
DWORD
WlHostLoggedOnSas(
    _In_ PWLSESSION Session)
{
    HANDLE Waits[2];
    DWORD Result = WLX_SAS_ACTION_NONE;
    DWORD Flags = WlHlOptionsFlags(Session);

    if (g_SubmitEvent == NULL)
        return WLX_SAS_ACTION_NONE;
    WlHostOnSas();
    WlHlStopHost();
    if (!WlHlEnsureHost(0, 0, WLHL_READY_MS))
        return WLX_SAS_ACTION_NONE;

    EnterCriticalSection(&g_SubmitLock);
    g_OptionsFlags = Flags;
    g_PendingAction = 0;
    g_OptionsUp = TRUE;
    LeaveCriticalSection(&g_SubmitLock);
    EnterCriticalSection(&g_LaunchLock);
    g_HostScenario = WLHL_SCENARIO_OPTIONS;
    LeaveCriticalSection(&g_LaunchLock);
    WlHlPost(WluiCmdShowOptions, WLHL_SCENARIO_OPTIONS, Flags, 0, 0, Session->UserName, NULL);
    LogonStage("options-shown");

    for (;;)
    {
        DWORD Which, Action;

        Waits[0] = g_SubmitEvent;
        Waits[1] = WlHostEndedEvent(g_Launch);
        Which = MsgWaitForMultipleObjects(2, Waits, FALSE, INFINITE, QS_ALLINPUT);
        if (Which == WAIT_OBJECT_0 + 2)
        {
            MSG Msg;
            BOOL Quit = FALSE;
            while (!Quit && PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            {
                if (Msg.message == WM_QUIT)
                {
                    PostQuitMessage((int)Msg.wParam);   /* belongs to winlogon's main loop */
                    Quit = TRUE;
                    break;
                }
                TranslateMessage(&Msg);
                DispatchMessageW(&Msg);
            }
            /* A logoff or shutdown notification ran nested in here, or the host went away. */
            if (Quit || Session->LogonState != STATE_LOGGED_ON_SAS || g_Launch == NULL)
                break;
            continue;
        }
        if (Which == WAIT_OBJECT_0 + 1)
            break;                      /* the host ended: back to the desktop */

        EnterCriticalSection(&g_SubmitLock);
        Action = g_PendingAction;
        g_PendingAction = 0;
        LeaveCriticalSection(&g_SubmitLock);
        if (Action == 0)
            continue;
        if (Action == WluiActionChangePassword)
        {
            /* The change page for the session's own account (WlHlChangePrefill names it); a submit
             * is taken only for that page (WlHlScenarioExpected), Cancel still comes back here. */
            EnterCriticalSection(&g_SubmitLock);
            g_ChangeFor = WLHL_CHANGE_SIGNED_IN;
            LeaveCriticalSection(&g_SubmitLock);
            if (!WlHlEnsureHost(WLHL_CPUS_CHANGE_PASSWORD, WLUI_FLAG_NO_POWER, WLHL_READY_MS))
                break;
            continue;
        }
        if (Action == WLHL_ACTION_CHANGED)
        {
            /* Leave the result on screen for a moment, as the change confirmation, still pumping
             * the SAS window (a logoff or shutdown notification may arrive meanwhile). */
            ULONGLONG Until = GetTickCount64() + WLHL_CHANGED_MS;
            while (g_Launch != NULL && Session->LogonState == STATE_LOGGED_ON_SAS)
            {
                ULONGLONG Now = GetTickCount64();
                HANDLE Ended = WlHostEndedEvent(g_Launch);
                MSG Msg;
                BOOL Quit = FALSE;

                if (Now >= Until ||
                    MsgWaitForMultipleObjects(1, &Ended, FALSE, (DWORD)(Until - Now), QS_ALLINPUT) != WAIT_OBJECT_0 + 1)
                    break;
                while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
                {
                    if (Msg.message == WM_QUIT)
                    {
                        PostQuitMessage((int)Msg.wParam);
                        Quit = TRUE;
                        break;
                    }
                    TranslateMessage(&Msg);
                    DispatchMessageW(&Msg);
                }
                if (Quit)
                    break;
            }
            break;
        }
        if (Action == WluiActionLock)
            Result = WLX_SAS_ACTION_LOCK_WKSTA;
        else if (Action == WluiActionLogoff)
            Result = WLX_SAS_ACTION_LOGOFF;
        else if (Action == WluiActionTaskManager)
            Result = WLX_SAS_ACTION_TASKLIST;
        else if (Action == WluiActionShutdown)
            Result = WLX_SAS_ACTION_SHUTDOWN_POWER_OFF;
        else if (Action == WluiActionRestart)
            Result = WLX_SAS_ACTION_SHUTDOWN_REBOOT;
        else if (Action == WluiActionSleep)
            Result = WLX_SAS_ACTION_SHUTDOWN_SLEEP;
        else if (Action == WluiActionEmergencyRestart)
            WlHlEmergencyRestart();
        break;                          /* Cancel and anything else: back to the desktop */
    }

    EnterCriticalSection(&g_SubmitLock);
    g_OptionsUp = FALSE;
    g_PendingAction = 0;
    if (g_ChangeFor == WLHL_CHANGE_SIGNED_IN)
        g_ChangeFor = 0;
    LeaveCriticalSection(&g_SubmitLock);
    WlHlStopHost();
    return Result;
}

/* ---- lock, unlock, screen saver (no thread waits: events come back through the SAS window) ---- */

/* P2: the unlock page is requested and waited for within a bound; the caller switches to the
 * Winlogon desktop whether or not the host made it, so the lock never depends on the host. */
static VOID
WlHlRequestUnlock(
    _In_ DWORD Flags)
{
    /* The station is locked: its user is signed in and could lose work if the power flyout is used. */
    if (!WlHlEnsureHost(WLHL_CPUS_UNLOCK, (WlHlPolicyFlags() & WLUI_FLAG_NO_POWER) | WLUI_FLAG_USERS_SIGNED_IN | Flags,
                        WlHlLockReadyMs()))
        ERR("WL: no logon UI for the locked station; it stays locked without a page\n");
}

/*
 * As on the sign-in screen: with CAD optional the unlock page comes up at once (behind the
 * host's curtain); with CAD required the station shows only the Ctrl+Alt+Delete notice, and
 * the unlock page waits for the user's SAS (WlHostLockedSas).
 */
VOID
WlHostLock(
    _In_ PWLSESSION Session)
{
    UNREFERENCED_PARAMETER(Session);
    if (WlHlDisableCad())
    {
        WlHlRequestUnlock(WLUI_FLAG_DISABLE_CAD);
        return;
    }
    /* A host showing a page (sign-in or unlock) gives way to the notice; a status-only host is kept. */
    if (WlHlHostLive() && g_HostScenario != 0)
        WlHlStopHost();
    if (!WlHlHostLive() && !WlHlEnsureHost(0, 0, WlHlLockReadyMs()))
    {
        ERR("WL: no logon UI for the locked notice; the station stays locked\n");
        return;
    }
    /* The signed-in flag marks the lock curtain, which comes up from black as on Windows 11. */
    WlHlPostResource(WluiCmdDisplayStatus, WLUI_FLAG_CURTAIN | WLUI_FLAG_USERS_SIGNED_IN,
                     IDS_PRESSCTRLALTDELETEUNLOCK);
}

/* The SAS on the locked station: with CAD required, now the unlock page may come up. */
VOID
WlHostLockedSas(
    _In_ PWLSESSION Session)
{
    UNREFERENCED_PARAMETER(Session);
    WlHostOnSas();                      /* a user SAS resets the levels, as on the sign-in screen */
    /* A live unlock page is kept as it is; a lost one comes back. */
    WlHlRequestUnlock(WlHlDisableCad() ? WLUI_FLAG_DISABLE_CAD : 0);
}

/* A logoff that could not start: the user is still signed in, so the status must go. */
VOID
WlHostLogoffAborted(
    _In_ PWLSESSION Session)
{
    WlHlStopHost();
    SwitchDesktop(Session->ApplicationDesktop);
}

/* The locked station asked for a password change before it may be unlocked. */
VOID
WlHostMustChange(
    _In_ PWLSESSION Session)
{
    UNREFERENCED_PARAMETER(Session);
    EnterCriticalSection(&g_SubmitLock);
    g_ChangeFor = WLHL_CPUS_UNLOCK;
    LeaveCriticalSection(&g_SubmitLock);
    WlHlEnsureHost(WLHL_CPUS_CHANGE_PASSWORD, WLUI_FLAG_FORCED_CHANGE | WLUI_FLAG_USERS_SIGNED_IN |
                   (WlHlPolicyFlags() & WLUI_FLAG_NO_POWER),
                   WlHlLockReadyMs());
}

/* The live host left outside a logged-out SAS: bring back the page the state needs. */
VOID
WlHostRelaunch(
    _In_ PWLSESSION Session)
{
    if (WlHlHostLive())
        return;                         /* a stale notice from a host we replaced ourselves */

    switch (Session->LogonState)
    {
        case STATE_LOCKED:
        case STATE_LOCKED_SAS:
            WlHostLock(Session);        /* §8.10 b: the station stays locked whatever comes up */
            break;
        case STATE_LOGGED_OFF:
            WlHlStopHost();
            WlHostDisplaySasNotice(Session);
            break;
        default:
            break;                      /* LOGGED_OFF_SAS: the waiting loop owns it; logged on: nothing to show */
    }
}

/* msgina WlxScreenSaverNotify: policy first, then the user's own setting under impersonation. */
BOOL
WlHostScreenSaverSecure(
    _In_ PWLSESSION Session)
{
    WCHAR Text[8];
    DWORD Value = 0, Size = sizeof(Text), Type;
    HKEY hUser, hKey;
    BOOL Secure = FALSE;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Control Panel\\Desktop",
                     L"ScreenSaverIsSecure", RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &Type, Text, &Size) == ERROR_SUCCESS)
        return Type == REG_DWORD ? *(PDWORD)Text != 0 : _wtoi(Text) != 0;

    if (Session->UserToken == NULL || !ImpersonateLoggedOnUser(Session->UserToken))
        return FALSE;
    if (RegOpenCurrentUser(KEY_READ, &hUser) == ERROR_SUCCESS)
    {
        if (RegOpenKeyExW(hUser, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
        {
            Size = sizeof(Text) - sizeof(WCHAR);
            if (RegQueryValueExW(hKey, L"ScreenSaverIsSecure", NULL, &Type, (PBYTE)Text, &Size) == ERROR_SUCCESS)
            {
                Text[Size / sizeof(WCHAR)] = UNICODE_NULL;
                Secure = (Type == REG_DWORD) ? (*(PDWORD)Text != 0) : (_wtoi(Text) != 0);
            }
            RegCloseKey(hKey);
        }
        RegCloseKey(hUser);
    }
    RevertToSelf();
    UNREFERENCED_PARAMETER(Value);
    return Secure;
}

/* msgina WlxIsLockOk: DisableLockWorkstation in HKLM, or in the session user's own hive under impersonation. */
BOOL
WlHostIsLockOk(
    _In_ PWLSESSION Session)
{
    DWORD Value = 0, Size = sizeof(Value);
    HKEY hUser;
    BOOL Ok = TRUE;

    if (WlHlPolicyDword(L"DisableLockWorkstation", 0) != 0)
        Ok = FALSE;
    else if (Session->UserToken != NULL && ImpersonateLoggedOnUser(Session->UserToken))
    {
        if (RegOpenCurrentUser(KEY_READ, &hUser) == ERROR_SUCCESS)
        {
            if (RegGetValueW(hUser, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                             L"DisableLockWorkstation", RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS &&
                Value != 0)
                Ok = FALSE;
            RegCloseKey(hUser);
        }
        RevertToSelf();
    }
    if (!Ok)
        ERR("WL: lock refused: DisableLockWorkstation is set\n");
    return Ok;
}

/* ---- status and hand-offs used by HandleLogon, HandleLogoff and HandleShutdown ------- */

/* Windows 11 wording (INVESTIGACION.md 2.3, 0b/0b+): the logon steps stay under "Welcome", the
 * logoff steps read "Signing out". 0 keeps what the host shows. */
UINT
WlHostStatusText(
    _In_ UINT ResourceId)
{
    switch (ResourceId)
    {
        case IDS_PREPARENETWORKCONNECTIONS:
        case IDS_APPLYINGCOMPUTERSETTINGS:
        case IDS_LOADINGYOURPERSONALSETTINGS:
        case IDS_APPLYINGYOURPERSONALSETTINGS:
        case IDS_RUNNINGLOGONSCRIPTS:
            return 0;

        case IDS_LOGGINGOFF:
        case IDS_SAVEYOURSETTINGS:
        case IDS_CLOSINGNETWORKCONNECTIONS:
        case IDS_RUNNINGLOGOFFSCRIPTS:
            return IDS_SIGNINGOUT;
    }
    return ResourceId;
}

/* A status page needs a host; logoff and shutdown start one when none is up (bounded, P2). */
VOID
WlHostStatus(
    _In_ PCWSTR Text)
{
    if (!WlHlHostLive() && !WlHlEnsureHost(0, 0, WlHlLockReadyMs()))
        return;
    g_StatusSequence = WlHlPost(WluiCmdDisplayStatus, g_HostScenario, WLUI_FLAG_SPINNER, 0, 0, NULL, Text);
}

/* Before the screen switches to the host, its status page must be on screen. */
VOID
WlHostWaitStatusShown(VOID)
{
    WlHlWaitShown(g_StatusSequence);
}

/* Unlock: the frame that crosses to the user's desktop shows Welcome, never the credential page.
 * FALSE when that is not confirmed; the caller then switches without a cover. */
BOOL
WlHostShowUnlockWelcome(
    _In_ PWLSESSION Session)
{
    if (!WlHlHostLive())
        return FALSE;
    return WlHlWaitShown(WlHlPost(WluiCmdDisplayWelcome, g_HostScenario, WLUI_FLAG_SPINNER, 0, 0, Session->UserName, NULL));
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

/* msgina's InitThemeSupport also called shsvcs ordinal 1 (ThemeWatchForStart): when the Themes
 * service signals its start event, that process installs the session's theme hooks and stays
 * their server (win32k drops them when its last GUI thread exits). Without it no process ever
 * installed them in host mode and every window was classic (VM leg finding 4). The module stays
 * loaded: it owns the registered wait and its callback. */
VOID
WlHostThemeWatch(VOID)
{
    typedef BOOL (WINAPI *THEMEWATCH)(VOID);
    HMODULE Module = LoadLibraryW(L"shsvcs.dll");
    THEMEWATCH Watch;

    if (Module == NULL)
    {
        ERR("WL: shsvcs.dll not loaded (error %lu); the session gets no theme hooks\n", GetLastError());
        return;
    }
    Watch = (THEMEWATCH)GetProcAddress(Module, (LPCSTR)1);
    if (Watch == NULL || !Watch())
        ERR("WL: ThemeWatchForStart unavailable or failed; the session gets no theme hooks\n");
}

/* ---- the desktop switch (PHASE3-PLAN §6.3.1, commit 8) ---------------------------------- */

/* ShellDesktopSwitchEvent for this logon: SYSTEM owns it, the user may only signal it. */
VOID
WlHostCreateDesktopSwitchEvent(
    _In_ PWLSESSION Session)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    PSID SystemSid = NULL;
    PTOKEN_USER User = NULL;
    PACL Dacl = NULL;
    SECURITY_DESCRIPTOR Sd;
    SECURITY_ATTRIBUTES Sa;
    DWORD Length = 0, AclSize;

    WlHlCloseSwitchHandles();
    if (Session->UserToken == NULL)
        return;

    GetTokenInformation(Session->UserToken, TokenUser, NULL, 0, &Length);
    User = LocalAlloc(LMEM_FIXED, Length);
    if (User == NULL || !GetTokenInformation(Session->UserToken, TokenUser, User, Length, &Length))
        goto done;
    if (!AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &SystemSid))
        goto done;

    AclSize = sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
              GetLengthSid(SystemSid) + GetLengthSid(User->User.Sid);
    Dacl = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, AclSize);
    if (Dacl == NULL ||
        !InitializeAcl(Dacl, AclSize, ACL_REVISION) ||
        !AddAccessAllowedAce(Dacl, ACL_REVISION, EVENT_ALL_ACCESS, SystemSid) ||
        !AddAccessAllowedAce(Dacl, ACL_REVISION, EVENT_MODIFY_STATE, User->User.Sid) ||
        !InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&Sd, TRUE, Dacl, FALSE))
        goto done;

    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = &Sd;
    Sa.bInheritHandle = FALSE;
    g_SwitchEvent = CreateEventW(&Sa, TRUE, FALSE, LIBERNT_SHELL_DESKTOP_SWITCH_EVENT);
    if (g_SwitchEvent != NULL && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        /* Not ours, so not trusted: the switch then relies on the timeout alone. */
        ERR("WL: %ls already exists; ignoring it\n", LIBERNT_SHELL_DESKTOP_SWITCH_EVENT);
        CloseHandle(g_SwitchEvent);
        g_SwitchEvent = NULL;
    }

done:
    if (g_SwitchEvent == NULL)
        ERR("WL: no desktop switch event (error %lu); the switch will wait for the timeout\n", GetLastError());
    if (Dacl) LocalFree(Dacl);
    if (SystemSid) FreeSid(SystemSid);
    if (User) LocalFree(User);
}

static DWORD
WlHlDesktopSwitchTimeoutMs(VOID)
{
    DWORD Seconds = WlHlPolicyDword(L"DelayedDesktopSwitchTimeout", LIBERNT_DESKTOP_SWITCH_TIMEOUT_DEFAULT_S);
    if (Seconds > WLHL_SWITCH_CAP_S)
        Seconds = LIBERNT_DESKTOP_SWITCH_TIMEOUT_DEFAULT_S;
    return Seconds * 1000;
}

/* Handles of the logon in progress; a logon that failed before the switch leaves them behind. */
static VOID
WlHlCloseSwitchHandles(VOID)
{
    if (g_SwitchEvent)
    {
        CloseHandle(g_SwitchEvent);
        g_SwitchEvent = NULL;
    }
    if (g_ShellProcess)
    {
        CloseHandle(g_ShellProcess);
        g_ShellProcess = NULL;
    }
}

/* Waits for the first of: explorer's signal, a Userinit process that failed, the timeout.
 * A Userinit that simply exited (it does, after starting the shell) keeps the wait going. */
VOID
WlHostWaitDesktopSwitch(
    _In_ PWLSESSION Session)
{
    HANDLE Waits[2];
    DWORD Count, Timeout = WlHlDesktopSwitchTimeoutMs();
    ULONGLONG Start = GetTickCount64();
    PCSTR Reason = "desktop-switch-timeout";
    BOOL TimedOut = TRUE;
    MSG Msg;

    UNREFERENCED_PARAMETER(Session);

    for (;;)
    {
        ULONGLONG Elapsed = GetTickCount64() - Start;
        DWORD Left = Elapsed < Timeout ? (DWORD)(Timeout - Elapsed) : 0, Result, ExitCode = 0;

        Count = 0;
        if (g_SwitchEvent) Waits[Count++] = g_SwitchEvent;
        if (g_ShellProcess) Waits[Count++] = g_ShellProcess;
        if (Count == 0 && Left == 0)
            break;

        /* Sent messages keep flowing to the SAS window while we wait (as WaitForUserDesktopReady did). */
        PeekMessageW(&Msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        Result = MsgWaitForMultipleObjects(Count, Waits, FALSE, Left, QS_SENDMESSAGE);
        if (Result == WAIT_OBJECT_0 + Count)
            continue;
        if (Result == WAIT_TIMEOUT)
            break;
        if (Result == WAIT_FAILED)
        {
            Reason = "desktop-switch-wait-failed";
            TimedOut = FALSE;
            break;
        }
        if (Count > 0 && Waits[Result - WAIT_OBJECT_0] == g_SwitchEvent)
        {
            Reason = "desktop-switch-signalled";
            TimedOut = FALSE;
            break;
        }
        /* Userinit ended: a failure switches now (nothing will ever signal); success keeps waiting. */
        GetExitCodeProcess(g_ShellProcess, &ExitCode);
        CloseHandle(g_ShellProcess);
        g_ShellProcess = NULL;
        if (ExitCode != 0)
        {
            Reason = "desktop-switch-shell-failed";
            TimedOut = FALSE;
            break;
        }
    }

    if (TimedOut)
        WARN("WL: desktop switch after %lu ms without the shell's signal\n", Timeout);
    TRACE("WL: %s after %I64u ms\n", Reason, GetTickCount64() - Start);
    LogonStage(Reason);                 /* the leg reads the switch from COM1 (checklist B.3) */
    WlHlCloseSwitchHandles();
}

/* The desktop behind is painted: the host starts its fade and the switch follows at once. */
VOID
WlHostBeforeDesktopSwitch(VOID)
{
    if (!WlHlHostLive())
        return;
    WlHlPost(WluiCmdReadyForDesktopSwitch, g_HostScenario, 0, 0, 0, NULL, NULL);
}

/* The host leaves by itself once its fade is done (0b+: about 1.6 s after the crossing); it is
 * only stopped if it overstays. */
VOID
WlHostAfterDesktopSwitch(VOID)
{
    if (WlHlHostLive())
        WaitForSingleObject(WlHostEndedEvent(g_Launch), WLHL_HOST_EXIT_MS);
    WlHlStopHost();
}

/* A logon that failed after authentication: tell the host, keep it for the next attempt. */
VOID
WlHostLogonFailed(
    _In_ NTSTATUS Status)
{
    WlHlCloseSwitchHandles();
    WlHlPost(WluiCmdReportResult, g_HostScenario, 0, Status, 0, NULL, NULL);
}

/* ---- WLX duties absorbed from msgina --------------------------------------------- */

static BOOL
WlHlRunAsUser(
    _In_ PWLSESSION Session,
    _In_ PCWSTR Desktop,
    _In_opt_ PVOID Environment,
    _In_ PCWSTR Command,
    _Out_opt_ PHANDLE Process)
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
    if (Process)
        *Process = Pi.hProcess;
    else
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
    return WlHlRunAsUser(Session, Desktop, Environment, Command, NULL);
}

/* The name the user signed in with, under the user's Explorer key (msgina WlxActivateUserShell). */
static VOID
WlHlStoreLogonUserName(
    _In_ PWLSESSION Session)
{
    PCWSTR Name = Session->UserName;
    HKEY User = NULL, Key;

    if (Name == NULL || Name[0] == UNICODE_NULL || Session->UserToken == NULL ||
        !ImpersonateLoggedOnUser(Session->UserToken))
        return;
    if (RegOpenCurrentUser(KEY_CREATE_SUB_KEY | KEY_SET_VALUE, &User) != ERROR_SUCCESS)
        User = NULL;
    if (User != NULL &&
        RegCreateKeyExW(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &Key, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(Key, L"Logon User Name", 0, REG_SZ, (const BYTE *)Name,
                       (DWORD)((wcslen(Name) + 1) * sizeof(WCHAR)));
        RegCloseKey(Key);
    }
    if (User != NULL)
        RegCloseKey(User);
    RevertToSelf();
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
        /* The first command (Userinit) is watched by the desktop switch for an early failure. */
        if (WlHlRunAsUser(Session, L"Default", Environment, Command, Any ? NULL : &g_ShellProcess))
            Any = TRUE;
    }
    if (Any)
        WlHlStoreLogonUserName(Session);
    return Any;
}
