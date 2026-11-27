/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Drives logonuihost_test.exe through a private wlui server
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <sddl.h>
#include <lm.h>
#include <strsafe.h>
#include <wluiclient.h>
#include <wluiauth.h>

extern RPC_IF_HANDLE wlui_v1_0_s_ifspec;

/* ---- the test server ------------------------------------------------------ */

static PWLUI_LAUNCH_STATE g_State;      /* command queue the host pulls from */
static HANDLE g_ReadyEvent, g_ExitEvent;
static LONG g_Heartbeats;
static LONG g_Faults;
static GUID g_FaultClsid;
static DWORD g_ExitReason = 0xFFFFFFFF;
static GUID g_ExpectedId;
static LONG g_WrongId;

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
    return ERROR_NOT_READY;
}

DWORD __stdcall WluiRequestAction(handle_t h, GUID LaunchId, WLUI_ACTION Action) { return ERROR_NOT_READY; }

DWORD __stdcall WluiReportProviderFault(handle_t h, GUID LaunchId, GUID ProviderClsid, DWORD Reason)
{
    CheckId(&LaunchId);
    g_FaultClsid = ProviderClsid;
    InterlockedIncrement(&g_Faults);
    return ERROR_SUCCESS;
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
HostPath(PWSTR Path, SIZE_T Count)
{
    PWSTR Slash;
    if (!GetModuleFileNameW(NULL, Path, (DWORD)Count))
        return FALSE;
    Slash = wcsrchr(Path, L'\\');
    if (!Slash)
        return FALSE;
    Slash[1] = UNICODE_NULL;
    return SUCCEEDED(StringCchCatW(Path, Count, L"logonuihost_test.exe"));
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

/* Launches the host with the given extra arguments; returns the process handle. */
static HANDLE
LaunchHost(PCWSTR Exe, PCWSTR Endpoint, PSID Own, PCWSTR Trace, PCWSTR Extra)
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
    StringCchPrintfW(Cmd, ARRAYSIZE(Cmd), L"\"%s\" /launchid:%s /scenario:1 /endpoint:%s /expect-server-sid:%s /trace:%s %s",
                     Exe, Id, Endpoint, SidString, Trace, Extra ? Extra : L"");
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
PostCmd(WLUI_COMMAND_TYPE Type, DWORD Flags, PCWSTR Text, PCWSTR User)
{
    WLUI_COMMAND Cmd;
    ZeroMemory(&Cmd, sizeof(Cmd));
    Cmd.Type = Type;
    Cmd.Flags = Flags;
    if (Text) StringCchCopyW(Cmd.Text, ARRAYSIZE(Cmd.Text), Text);
    if (User) StringCchCopyW(Cmd.User, ARRAYSIZE(Cmd.User), User);
    ok(WluiStatePost(g_State, &Cmd, sizeof(Cmd)), "post %u\n", Type);
}

/* One launch state per run, as winlogon keeps one per launch: the previous host's abandoned
 * WaitForCommand can sit in the old state for up to 2 s and would take the next command for a
 * connection that is gone. The old state is marked dead (the stale wait returns
 * ERROR_OPERATION_ABORTED) and left to process exit, since that handler may still be inside it. */
static void
ResetRun(void)
{
    ResetEvent(g_ReadyEvent);
    ResetEvent(g_ExitEvent);
    g_Heartbeats = 0;
    g_ExitReason = 0xFFFFFFFF;
    if (g_State)
        WluiStateMarkDead(g_State);
    g_State = WluiStateCreate(sizeof(WLUI_COMMAND));
}

/* ---- credential providers in the host ---------------------------------------- */

/* The test provider and its CLSID key, as the credprov Framework test registers them. Its GUID
 * string sorts before the PasswordProvider's, so the host selects its credential first. */
#include <initguid.h>
DEFINE_GUID(CLSID_CredProvTestProvider, 0x5e0a4b7c, 0x2d3f, 0x4c61, 0x9a, 0x0e, 0x7b, 0x21, 0x3c, 0x94, 0xd8, 0x01);
#define AUTH_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication"
#define PROVIDERS_KEY AUTH_KEY L"\\Credential Providers"

static BOOL
ClsidString(REFCLSID Clsid, PWSTR Out, SIZE_T Count)
{
    return StringFromGUID2(Clsid, Out, (int)Count) > 0;
}

static BOOL
RegisterTestProvider(BOOL Register)
{
    WCHAR Guid[40], Key[256], Dll[MAX_PATH];
    HKEY h;
    PWSTR Slash;

    ClsidString(&CLSID_CredProvTestProvider, Guid, ARRAYSIZE(Guid));
    if (!GetModuleFileNameW(NULL, Dll, ARRAYSIZE(Dll)) || !(Slash = wcsrchr(Dll, L'\\')))
        return FALSE;
    Slash[1] = UNICODE_NULL;
    StringCchCatW(Dll, ARRAYSIZE(Dll), L"credprov_testmod.dll");
    if (GetFileAttributesW(Dll) == INVALID_FILE_ATTRIBUTES)
        return FALSE;

    StringCchPrintfW(Key, ARRAYSIZE(Key), L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", Guid);
    if (!Register)
    {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, Key);
        StringCchPrintfW(Key, ARRAYSIZE(Key), L"SOFTWARE\\Classes\\CLSID\\%s", Guid);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, Key);
        StringCchPrintfW(Key, ARRAYSIZE(Key), PROVIDERS_KEY L"\\%s", Guid);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, Key);
        return TRUE;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, Key, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS)
        return FALSE;
    RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE *)Dll, (DWORD)((wcslen(Dll) + 1) * sizeof(WCHAR)));
    RegSetValueExW(h, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
    RegCloseKey(h);
    StringCchPrintfW(Key, ARRAYSIZE(Key), PROVIDERS_KEY L"\\%s", Guid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, Key, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(h);
    return TRUE;
}

static void
Test_Providers(PCWSTR Exe, PCWSTR Endpoint, PSID Own, PCWSTR Trace)
{
    HANDLE Process;
    DWORD Code;
    BOOL Registered;

    /* Fallback: only the system PasswordProvider, and a page comes up. */
    ResetRun();
    DeleteFileW(Trace);
    Process = LaunchHost(Exe, Endpoint, Own, Trace, L"/fallback");
    ok(Process != NULL, "launch (fallback)\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (fallback)\n");
    PostCmd(WluiCmdRequestCredentials, WLUI_FLAG_FALLBACK, NULL, NULL);
    Sleep(3000);
    ok(TraceHas(Trace, "providers 1"), "fallback did not enumerate exactly one provider\n");
    ok(TraceHas(Trace, "page "), "fallback showed no credential page\n");
    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "fallback exit %lu\n", Code);

    RegisterTestProvider(FALSE);        /* a crashed earlier run must not leave it behind */
    Registered = RegisterTestProvider(TRUE);
    if (!Registered)
    {
        skip("cannot register credprov_testmod (not elevated, or not built)\n");
        return;
    }

    /* R1: a provider wedged during start-up (before any credential exists) cannot be named;
     * the host reports a start-up fault with CLSID_NULL and comes back in the fallback tier. */
    ResetRun();
    DeleteFileW(Trace);
    g_Faults = 0;
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", L"hang-startup");
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", NULL);
    ok(Process != NULL, "launch (hang-startup)\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (hang-startup)\n");
    PostCmd(WluiCmdRequestCredentials, 0, NULL, NULL);
    {
        DWORD Started = GetTickCount();
        while (g_Faults == 0 && GetTickCount() - Started < 15000)
            Sleep(200);
        Sleep(3000);    /* the fallback enumeration and its page */
    }
    ok(g_Faults == 1, "start-up fault reports %ld (expected 1)\n", g_Faults);
    ok(IsEqualGUID(&g_FaultClsid, &CLSID_NULL), "start-up fault CLSID is not CLSID_NULL\n");
    ok(TraceHas(Trace, "providers 1"), "fallback enumeration after the start-up fault not seen\n");
    ok(TraceHas(Trace, "page "), "no fallback page after the start-up fault\n");
    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "hang-startup run exit %lu\n", Code);

    /* R4: a provider that is merely slow delivers its page after the fallback took over;
     * that late page is dropped, the fallback page stands, and the host still exits 0. */
    ResetRun();
    DeleteFileW(Trace);
    g_Faults = 0;
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", L"slow-startup");
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", NULL);
    ok(Process != NULL, "launch (slow-startup)\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (slow-startup)\n");
    PostCmd(WluiCmdRequestCredentials, 0, NULL, NULL);
    Sleep(17000);   /* 10 s deadline, fallback page, then the slow thread's late page at ~13 s */
    ok(g_Faults == 1, "slow-startup fault reports %ld (expected 1)\n", g_Faults);
    ok(IsEqualGUID(&g_FaultClsid, &CLSID_NULL), "slow-startup fault CLSID is not CLSID_NULL\n");
    ok(TraceHas(Trace, "providers 1"), "no fallback enumeration after the slow start-up\n");
    ok(TraceHas(Trace, "page-dropped"), "the late page of the slow thread was not dropped\n");
    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "slow-startup run exit %lu (expected 0, not 3)\n", Code);

    /* A provider that hangs in GetSerialization is dropped within the call timeout; the
     * host reports it, stays alive and comes back with the remaining provider's page. */
    ResetRun();
    DeleteFileW(Trace);
    g_Faults = 0;
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", L"hang");
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", NULL);
    ok(Process != NULL, "launch (hang)\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (hang)\n");
    PostCmd(WluiCmdRequestCredentials, 0, NULL, NULL);
    Sleep(2500);
    ok(TraceHas(Trace, "page 3"), "test provider page not shown first\n");
    /* Submitting drives GetSerialization, which hangs. */
    {
        HWND Page = NULL;
        DWORD Started = GetTickCount();
        while (!Page && GetTickCount() - Started < 3000)
        {
            Page = FindWindowW(L"LiberNT_LogonUIHost", NULL);
            if (!Page) Sleep(100);
        }
        ok(Page != NULL, "host window not found\n");
        if (Page)
        {
            HWND Button = FindWindowExW(Page, NULL, L"BUTTON", L"Submit");
            ok(Button != NULL, "submit button not found\n");
            if (Button)
                PostMessageW(Page, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(Button), BN_CLICKED), (LPARAM)Button);
        }
    }
    {
        DWORD Started = GetTickCount();
        while (g_Faults == 0 && GetTickCount() - Started < 15000)
            Sleep(200);
    }
    ok(g_Faults == 1, "provider fault reports %ld (expected 1)\n", g_Faults);
    ok(IsEqualGUID(&g_FaultClsid, &CLSID_CredProvTestProvider), "fault CLSID is not the test provider\n");
    g_Heartbeats = 0;
    Sleep(4500);
    ok(g_Heartbeats >= 1, "host stopped heartbeating after the fault\n");
    ok(TraceHas(Trace, "provider-fault"), "fault not traced\n");
    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "hang run exit %lu\n", Code);

    /* A provider that faults takes the process down; the filter exits without a dump. */
    ResetRun();
    DeleteFileW(Trace);
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", L"crash");
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    SetEnvironmentVariableW(L"CREDPROV_TESTMOD_MODE", NULL);
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (crash)\n");
    PostCmd(WluiCmdRequestCredentials, 0, NULL, NULL);
    Sleep(2500);
    {
        HWND Page = FindWindowW(L"LiberNT_LogonUIHost", NULL);
        HWND Button = Page ? FindWindowExW(Page, NULL, L"BUTTON", L"Submit") : NULL;
        if (Button)
            PostMessageW(Page, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(Button), BN_CLICKED), (LPARAM)Button);
    }
    Code = WaitExit(Process, 15000);
    ok(Code == 4, "crash run exit %lu (expected 4)\n", Code);

    RegisterTestProvider(FALSE);
}

/* ---- tiles: the user array, the tile list, the curtain, the power button ------ */

/* The accounts the host should list: enabled normal local accounts not hidden by SpecialAccounts\UserList. */
static DWORD
ExpectedUsers(void)
{
    PUSER_INFO_1 Info;
    DWORD Read, Total, Resume = 0, i, Count = 0;
    NET_API_STATUS Status;

    do
    {
        Info = NULL;
        Read = 0;
        Status = NetUserEnum(NULL, 1, FILTER_NORMAL_ACCOUNT, (LPBYTE *)&Info, MAX_PREFERRED_LENGTH, &Read, &Total,
                             &Resume);
        if (Status != NERR_Success && Status != ERROR_MORE_DATA)
            break;
        for (i = 0; Info != NULL && i < Read; i++)
        {
            DWORD Value = 1, Size = sizeof(Value);

            if (Info[i].usri1_flags & UF_ACCOUNTDISABLE)
                continue;
            if (RegGetValueW(HKEY_LOCAL_MACHINE,
                             L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\SpecialAccounts\\UserList",
                             Info[i].usri1_name, RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS && Value == 0)
                continue;
            Count++;
        }
        if (Info != NULL)
            NetApiBufferFree(Info);
    } while (Status == ERROR_MORE_DATA && Read);
    return Count;
}

static void
PostScenario(DWORD Scenario, DWORD Flags, PCWSTR User)
{
    WLUI_COMMAND Cmd;
    ZeroMemory(&Cmd, sizeof(Cmd));
    Cmd.Type = WluiCmdRequestCredentials;
    Cmd.Scenario = Scenario;
    Cmd.Flags = Flags;
    if (User) StringCchCopyW(Cmd.User, ARRAYSIZE(Cmd.User), User);
    ok(WluiStatePost(g_State, &Cmd, sizeof(Cmd)), "post RequestCredentials\n");
}

static HWND
HostWindow(void)
{
    HWND Window = NULL;
    DWORD Started = GetTickCount();
    while (!Window && GetTickCount() - Started < 3000)
    {
        Window = FindWindowW(L"LiberNT_LogonUIHost", NULL);
        if (!Window) Sleep(100);
    }
    return Window;
}

/* Starts a host, asks for Scenario's credentials, leaves it up for the checks; Dismiss ends it. */
static HANDLE
StartTilesRun(PCWSTR Exe, PCWSTR Endpoint, PSID Own, PCWSTR Trace, DWORD Scenario, DWORD Flags, PCWSTR User)
{
    HANDLE Process;

    ResetRun();
    DeleteFileW(Trace);
    Process = LaunchHost(Exe, Endpoint, Own, Trace, L"/fallback");
    ok(Process != NULL, "launch (tiles)\n");
    if (Process == NULL)
        return NULL;
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (tiles)\n");
    PostScenario(Scenario, WLUI_FLAG_FALLBACK | Flags, User);
    Sleep(3000);
    return Process;
}

static void
EndTilesRun(HANDLE Process, PCSTR What)
{
    DWORD Code;

    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "%s run exit %lu\n", What, Code);
}

static void
Test_Tiles(PCWSTR Exe, PCWSTR Endpoint, PSID Own, PCWSTR Trace)
{
    static const PCWSTR PolicyKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    DWORD Users = ExpectedUsers();
    CHAR Needle[64];
    WCHAR Me[UNLEN + 1];
    DWORD MeChars = ARRAYSIZE(Me);
    HANDLE Process;
    HWND Window;
    BOOL Listed = FALSE;

    /* Logon: one tile per listed account plus Other user, and the power button. */
    Process = StartTilesRun(Exe, Endpoint, Own, Trace, 1, 0, NULL);
    if (Process)
    {
        StringCchPrintfA(Needle, ARRAYSIZE(Needle), "users %lu\r\n", Users);
        ok(TraceHas(Trace, Needle), "logon: expected '%s' in the trace\n", "users N");
        if (Users > 0)
        {
            StringCchPrintfA(Needle, ARRAYSIZE(Needle), "/%lu\r\n", Users + 1);
            ok(TraceHas(Trace, "tiles ") && TraceHas(Trace, Needle), "logon: expected %lu tiles\n", Users + 1);
        }
        Window = HostWindow();
        ok(Window != NULL && FindWindowExW(Window, NULL, L"BUTTON", L"Power") != NULL, "logon: no power button\n");
        EndTilesRun(Process, "logon");
    }

    /* ShutdownWithoutLogon=0: no power button. */
    Process = StartTilesRun(Exe, Endpoint, Own, Trace, 1, WLUI_FLAG_NO_POWER, NULL);
    if (Process)
    {
        Window = HostWindow();
        ok(Window != NULL, "no-power: host window not found\n");
        ok(Window == NULL || FindWindowExW(Window, NULL, L"BUTTON", L"Power") == NULL, "no-power: power button shown\n");
        EndTilesRun(Process, "no-power");
    }

    /* DontDisplayLastUserName: no account is listed, only the empty tile. */
    {
        HKEY Key;
        DWORD Old = 0, OldSize = sizeof(Old), One = 1;
        BOOL HadOld;

        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PolicyKey, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &Key,
                            NULL) != ERROR_SUCCESS)
        {
            skip("cannot open the System policy key\n");
        }
        else
        {
            HadOld = RegQueryValueExW(Key, L"DontDisplayLastUserName", NULL, NULL, (PBYTE)&Old, &OldSize) == ERROR_SUCCESS;
            ok(RegSetValueExW(Key, L"DontDisplayLastUserName", 0, REG_DWORD, (PBYTE)&One, sizeof(One)) == ERROR_SUCCESS,
               "set DontDisplayLastUserName\n");
            Process = StartTilesRun(Exe, Endpoint, Own, Trace, 1, 0, NULL);
            if (Process)
            {
                ok(TraceHas(Trace, "users 0\r\n"), "DontDisplayLastUserName: accounts were listed\n");
                ok(!TraceHas(Trace, "tiles "), "DontDisplayLastUserName: a tile list was shown\n");
                EndTilesRun(Process, "hide-last");
            }
            if (HadOld)
                RegSetValueExW(Key, L"DontDisplayLastUserName", 0, REG_DWORD, (PBYTE)&Old, sizeof(Old));
            else
                RegDeleteValueW(Key, L"DontDisplayLastUserName");
            RegCloseKey(Key);
        }
    }

    /* CAD off: the curtain comes first; the page waits under it until a key. */
    Process = StartTilesRun(Exe, Endpoint, Own, Trace, 1, WLUI_FLAG_DISABLE_CAD, NULL);
    if (Process)
    {
        ok(TraceHas(Trace, "curtain"), "CAD off: no curtain\n");
        ok(!TraceHas(Trace, "page "), "CAD off: the page came up under the curtain\n");
        Window = HostWindow();
        if (Window)
            PostMessageW(Window, WM_KEYDOWN, VK_SPACE, 0);
        Sleep(1000);
        ok(TraceHas(Trace, "curtain-lifted"), "CAD off: a key did not lift the curtain\n");
        ok(TraceHas(Trace, "page "), "CAD off: no page after the curtain\n");
        EndTilesRun(Process, "curtain");
    }

    /* CAD required: no curtain, the page at once. */
    Process = StartTilesRun(Exe, Endpoint, Own, Trace, 1, 0, NULL);
    if (Process)
    {
        ok(!TraceHas(Trace, "curtain"), "CAD on: a curtain was shown\n");
        ok(TraceHas(Trace, "page "), "CAD on: no page\n");
        EndTilesRun(Process, "cad");
    }

    /* Unlock: only the named account; without a name, none. */
    if (GetUserNameW(Me, &MeChars))
    {
        PUSER_INFO_1 Info = NULL;

        /* Only an enabled local account (not SYSTEM) can be the one the host lists. */
        Listed = NetUserGetInfo(NULL, Me, 1, (LPBYTE *)&Info) == NERR_Success && Info != NULL &&
                 !(Info->usri1_flags & UF_ACCOUNTDISABLE);
        if (Info)
            NetApiBufferFree(Info);
    }
    if (!Listed)
    {
        skip("unlock: the test account is not a listed local account\n");
    }
    else
    {
        Process = StartTilesRun(Exe, Endpoint, Own, Trace, 2, 0, Me);
        if (Process)
        {
            ok(TraceHas(Trace, "users 1\r\n"), "unlock: the locked account was not the only one listed\n");
            EndTilesRun(Process, "unlock");
        }
    }
    Process = StartTilesRun(Exe, Endpoint, Own, Trace, 2, 0, NULL);
    if (Process)
    {
        ok(TraceHas(Trace, "users 0\r\n"), "unlock without a name: accounts were listed\n");
        EndTilesRun(Process, "unlock-noname");
    }
}

/* ---- tests ------------------------------------------------------------------ */

START_TEST(Host)
{
    WCHAR Exe[MAX_PATH], Endpoint[64], Trace[MAX_PATH], Temp[MAX_PATH];
    PSID Own = GetOwnSid();
    HANDLE Process;
    DWORD Code;

    if (!VisibleWindowStation())
    {
        skip("no visible window station; the host cannot create its window here\n");
        return;
    }
    ok(HostPath(Exe, ARRAYSIZE(Exe)), "host path\n");
    if (GetFileAttributesW(Exe) == INVALID_FILE_ATTRIBUTES)
    {
        skip("%ls not found\n", Exe);
        return;
    }
    ok(Own != NULL, "own sid\n");
    UuidCreate(&g_ExpectedId);
    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(Trace, ARRAYSIZE(Trace), L"%sluh-%lu.log", Temp, GetCurrentProcessId());
    StringCchPrintfW(Endpoint, ARRAYSIZE(Endpoint), L"wlui-luh-%lu", GetCurrentProcessId());

    g_State = WluiStateCreate(sizeof(WLUI_COMMAND));
    g_ReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

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

    /* No server behind the endpoint: the host must give up with the IPC code, not hang. */
    DeleteFileW(Trace);
    Process = LaunchHost(Exe, L"wlui-luh-nobody", Own, Trace, NULL);
    ok(Process != NULL, "launch (no server)\n");
    Code = WaitExit(Process, 10000);
    ok(Code == 2, "no server exit %lu\n", Code);

    if (!StartServer(Endpoint, Own))
    {
        skip("cannot start the test server\n");
        return;
    }

    /* Run 1: Ready after the first paint, heartbeats, status and welcome, then the desktop-switch fade. */
    ResetRun();
    DeleteFileW(Trace);
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    ok(Process != NULL, "launch 1\n");
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready\n");
    Sleep(4500);
    ok(g_Heartbeats >= 2, "heartbeats %ld\n", g_Heartbeats);
    PostCmd(WluiCmdDisplayStatus, WLUI_FLAG_SPINNER, L"Please wait", NULL);
    PostCmd(WluiCmdDisplayWelcome, 0, NULL, L"Alice");
    Sleep(1500);
    ok(TraceHas(Trace, "status Please wait"), "status not shown\n");
    ok(TraceHas(Trace, "welcome Alice"), "welcome not shown\n");
    PostCmd(WluiCmdReadyForDesktopSwitch, 0, NULL, NULL);
    ok(WaitForSingleObject(g_ExitEvent, 5000) == WAIT_OBJECT_0, "no Exiting after switch\n");
    ok(g_ExitReason == WluiExitAfterDesktopSwitch, "exit reason %lu\n", g_ExitReason);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "run 1 exit %lu\n", Code);

    /* Run 2: Dismiss. */
    ResetRun();
    Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
    ok(WaitForSingleObject(g_ReadyEvent, 5000) == WAIT_OBJECT_0, "no Ready (2)\n");
    PostCmd(WluiCmdDismiss, 0, NULL, NULL);
    ok(WaitForSingleObject(g_ExitEvent, 5000) == WAIT_OBJECT_0, "no Exiting after dismiss\n");
    ok(g_ExitReason == WluiExitDismissed, "dismiss reason %lu\n", g_ExitReason);
    Code = WaitExit(Process, 5000);
    ok(Code == 0, "run 2 exit %lu\n", Code);

    /* R1: a Dismiss always lands while the pull thread sits in a server wait; teardown must not
     * free the binding under that call. Twenty times in a row, the exit is 0, never 4. */
    {
        int Run, Bad = 0;
        for (Run = 0; Run < 20; Run++)
        {
            ResetRun();
            Process = LaunchHost(Exe, Endpoint, Own, Trace, NULL);
            if (Process == NULL || WaitForSingleObject(g_ReadyEvent, 5000) != WAIT_OBJECT_0)
            {
                Bad++;
                if (Process) WaitExit(Process, 1000);
                continue;
            }
            PostCmd(WluiCmdDismiss, 0, NULL, NULL);
            Code = WaitExit(Process, 5000);
            if (Code != 0 || g_ExitReason != WluiExitDismissed)
            {
                trace("dismiss run %d: exit %lu reason %lu\n", Run, Code, g_ExitReason);
                Bad++;
            }
        }
        ok(Bad == 0, "%d of 20 dismiss-under-wait runs failed\n", Bad);
    }

    /* A RequestCredentials brings up a page since the host got its credential providers
     * (Test_Providers covers it, and the not-supported exit when no tier can be instantiated). */
    ok(g_WrongId == 0, "%ld call(s) with a wrong launch id\n", g_WrongId);

    Test_Providers(Exe, Endpoint, Own, Trace);
    Test_Tiles(Exe, Endpoint, Own, Trace);

    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(wlui_v1_0_s_ifspec, NULL, TRUE);
    WluiStateRelease(g_State);
    CloseHandle(g_ReadyEvent);
    CloseHandle(g_ExitEvent);
    DeleteFileW(Trace);
}
