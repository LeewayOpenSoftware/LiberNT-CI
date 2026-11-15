/*
 * PROJECT:     LiberNT API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the elevation broker of the secondary logon service
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <winuser.h>
#include <lm.h>
#include <sddl.h>
#include <seclogon_c.h>

#ifndef TokenElevationTypeLimited
#define TokenElevationTypeLimited 3
#endif

#define ELEV_TEST_USER     L"LntElevationTest"
#define ELEV_TEST_PASSWORD L"Lnt!Elevation#2026q"
#define ELEV_POLICY_KEY    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"
#define ELEV_CONSENT_TITLE L"User Account Control"
#define ELEV_IMAGE_DIR     L"\\Temp\\LntElevation"

/* "WinSta0\\Default" style name of the window station and desktop this thread uses */
static
BOOL
GetCurrentDesktopName(PWSTR Buffer, DWORD Chars)
{
    WCHAR Station[128], Desk[128];
    DWORD Needed;

    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, Station, sizeof(Station), &Needed) ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, Desk, sizeof(Desk), &Needed))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(Buffer, Chars, L"%ls\\%ls", Station, Desk));
}

/* Child exit codes: the broker's error for the 'call' modes, a bit mask for 'elev' */
#define ELEV_CHILD_UNEXPECTED   0x10000000
#define ELEV_CHILD_PIPE_DENIED  0x20000000   /* 'call': the pipe itself refused the open */

static
DWORD
CallBroker(
    _In_opt_ PCWSTR CommandLine,
    _In_opt_ PCWSTR ApplicationName,
    _Out_ SECL_RESPONSE *Response)
{
    SECL_ELEVATE_REQUEST Request = { 0 };
    RPC_WSTR StringBinding = NULL;
    handle_t hBinding = NULL;
    DWORD dwError;

    WCHAR Desktop[256];

    ZeroMemory(Response, sizeof(*Response));
    Request.ApplicationName = (WCHAR *)ApplicationName;
    Request.CommandLine = (WCHAR *)CommandLine;
    Request.dwShowCmd = SW_HIDE;
    Request.dwCreationFlags = CREATE_NO_WINDOW;
    Request.dwDesktopInherited = TRUE;
    /* A service runner lives on its own window station; the prompt must come there */
    if (GetCurrentDesktopName(Desktop, _countof(Desktop)) && _wcsnicmp(Desktop, L"WinSta0\\", 8) != 0)
    {
        Request.Desktop = Desktop;
        Request.dwDesktopInherited = FALSE;
    }

    if (RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncacn_np", NULL, (RPC_WSTR)L"\\pipe\\seclogon", NULL,
                                 &StringBinding) != RPC_S_OK)
        return ERROR_SERVICE_NOT_ACTIVE;
    if (RpcBindingFromStringBindingW(StringBinding, &hBinding) != RPC_S_OK)
    {
        RpcStringFreeW(&StringBinding);
        return ERROR_SERVICE_NOT_ACTIVE;
    }
    RpcStringFreeW(&StringBinding);

    RpcTryExcept
    {
        SeclCreateProcessElevatedW(hBinding, &Request, Response);
        dwError = Response->dwError;
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        dwError = RpcExceptionCode();
    }
    RpcEndExcept;
    RpcBindingFree(&hBinding);

    if (dwError == RPC_S_SERVER_UNAVAILABLE || dwError == EPT_S_NOT_REGISTERED || dwError == RPC_S_PROCNUM_OUT_OF_RANGE)
        dwError = ERROR_SERVICE_NOT_ACTIVE;
    return dwError;
}

static
BOOL
GetTokenDword(HANDLE Token, TOKEN_INFORMATION_CLASS Class, DWORD *Value)
{
    DWORD Size;

    *Value = 0;
    return GetTokenInformation(Token, Class, Value, sizeof(*Value), &Size);
}

/* Runs a command with our own environment and returns its exit code (control run) */
static
DWORD
RunPlain(PWSTR Cmd)
{
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode = 0xffffffff;

    if (!CreateProcessW(NULL, Cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &StartupInfo, &ProcessInfo))
        return ExitCode;
    if (WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0)
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    else
        TerminateProcess(ProcessInfo.hProcess, 0);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    return ExitCode;
}

/* 'call': return the broker's answer for a harmless command.
 * 'callapp': the same for the image named by Extra (the prompt, when the policy asks for one).
 * 'elev': expect an elevated start and check what came back (bit mask).
 * 'imp': call while impersonating the inherited token of another logon. */
static
DWORD
RunChild(PCSTR Mode, HANDLE Inherited, PCWSTR Extra)
{
    SECL_RESPONSE Response;
    WCHAR SystemDir[MAX_PATH], Cmd[MAX_PATH + 64], Subst[MAX_PATH + 16];
    DWORD dwError, Failures = 0, Value, ExitCode = 0xffffffff;
    HANDLE hProcess, hToken;
    SIZE_T Written;
    BYTE Byte = 0;

    GetSystemDirectoryW(SystemDir, _countof(SystemDir));
    StringCchPrintfW(Cmd, _countof(Cmd), L"\"%ls\\cmd.exe\" /c set LNT_ELEVATION_TEST", SystemDir);

    if (!strcmp(Mode, "call"))
    {
        dwError = CallBroker(Cmd, NULL, &Response);
        if (dwError == ERROR_SERVICE_NOT_ACTIVE)
        {
            /* Not reachable: a service that is gone, or a pipe we may not open */
            hProcess = CreateFileW(L"\\\\.\\pipe\\seclogon", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hProcess == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
                return ELEV_CHILD_PIPE_DENIED;
            if (hProcess != INVALID_HANDLE_VALUE)
                CloseHandle(hProcess);
        }
        return dwError;
    }

    if (!strcmp(Mode, "callapp"))
    {
        StringCchPrintfW(Cmd, _countof(Cmd), L"\"%ls\" /c exit 0", Extra);
        return CallBroker(Cmd, Extra, &Response);
    }

    if (!strcmp(Mode, "imp"))
    {
        if (!SetThreadToken(NULL, Inherited))
            return ELEV_CHILD_UNEXPECTED | GetLastError();
        dwError = CallBroker(Cmd, NULL, &Response);
        if (dwError == ERROR_SERVICE_NOT_ACTIVE)
        {
            /* The pipe itself may already refuse the impersonating thread */
            hProcess = CreateFileW(L"\\\\.\\pipe\\seclogon", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hProcess == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
                dwError = ELEV_CHILD_PIPE_DENIED;
            else if (hProcess != INVALID_HANDLE_VALUE)
                CloseHandle(hProcess);
        }
        RevertToSelf();
        return dwError;
    }

    /* elev: a variable of ours must not reach the elevated side (cmd's 'set X' exits 1 when X is
     * undefined). The control run with our own environment must see it, or the check is void. */
    SetEnvironmentVariableW(L"LNT_ELEVATION_TEST", L"1");
    if (RunPlain(Cmd) != 0)
        Failures |= 0x100;
    dwError = CallBroker(Cmd, NULL, &Response);
    if (dwError != ERROR_SUCCESS)
        return ELEV_CHILD_UNEXPECTED | dwError;
    hProcess = (HANDLE)Response.hProcess;
    if (hProcess == NULL)
        return Failures | 0x1;
    if (Response.hThread != 0)
        Failures |= 0x2;

    /* The handle waits and queries, nothing more */
    if (WriteProcessMemory(hProcess, (PVOID)0x10000, &Byte, 1, &Written) || GetLastError() != ERROR_ACCESS_DENIED)
        Failures |= 0x4;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken))
    {
        if (!GetTokenDword(hToken, TokenElevation, &Value) || Value != 1)
            Failures |= 0x8;
        CloseHandle(hToken);
    }
    else
        Failures |= 0x10;
    if (WaitForSingleObject(hProcess, 30000) != WAIT_OBJECT_0)
        Failures |= 0x20;
    else if (!GetExitCodeProcess(hProcess, &ExitCode) || ExitCode != 1)
        Failures |= 0x40;
    CloseHandle(hProcess);

    /* An image on a drive letter of our own making is refused before any prompt */
    if (DefineDosDeviceW(0, L"Q:", SystemDir))
    {
        StringCchPrintfW(Subst, _countof(Subst), L"Q:\\cmd.exe");
        dwError = CallBroker(L"cmd.exe /c exit 0", Subst, &Response);
        /* Refused as a made-up drive, or never found from the broker's side: both keep it out */
        if (dwError != ERROR_BAD_PATHNAME && dwError != ERROR_FILE_NOT_FOUND && dwError != ERROR_PATH_NOT_FOUND)
            Failures |= 0x80;
        if (Response.hProcess != 0)
            CloseHandle((HANDLE)Response.hProcess);
        DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"Q:", NULL);
    }
    return Failures;
}

static
BOOL
StartChildAsync(HANDLE Token, PCSTR Mode, HANDLE Inherited, PCWSTR Extra, HANDLE *phChild)
{
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR Exe[MAX_PATH], CommandLine[2 * MAX_PATH + 64];
    HANDLE Dup = NULL;
    BOOL Ret;

    *phChild = NULL;
    if (!GetModuleFileNameW(NULL, Exe, _countof(Exe)))
        return FALSE;
    if (Inherited != NULL &&
        !DuplicateHandle(GetCurrentProcess(), Inherited, GetCurrentProcess(), &Dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return FALSE;
    StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" Elevation child %hs %p \"%ls\"", Exe, Mode, Dup,
                     Extra ? Extra : L"");
    Ret = CreateProcessAsUserW(Token, Exe, CommandLine, NULL, NULL, Dup != NULL, CREATE_NO_WINDOW, NULL, NULL,
                               &StartupInfo, &ProcessInfo);
    if (Ret)
    {
        CloseHandle(ProcessInfo.hThread);
        *phChild = ProcessInfo.hProcess;
    }
    if (Dup != NULL)
        CloseHandle(Dup);
    return Ret;
}

static
void
WaitChild(HANDLE hChild, DWORD Timeout, DWORD *ExitCode)
{
    *ExitCode = 0xffffffff;
    if (WaitForSingleObject(hChild, Timeout) == WAIT_OBJECT_0)
        GetExitCodeProcess(hChild, ExitCode);
    else
        TerminateProcess(hChild, 0);
    CloseHandle(hChild);
}

static
BOOL
StartChild(HANDLE Token, PCSTR Mode, HANDLE Inherited, DWORD *ExitCode)
{
    HANDLE hChild;

    *ExitCode = 0xffffffff;
    if (!StartChildAsync(Token, Mode, Inherited, NULL, &hChild))
        return FALSE;
    WaitChild(hChild, 60000, ExitCode);
    return TRUE;
}

/* Lowers a copy of our token to Low integrity */
static
HANDLE
MakeLowToken(HANDLE Token)
{
    SID_IDENTIFIER_AUTHORITY LabelAuthority = { SECURITY_MANDATORY_LABEL_AUTHORITY };
    TOKEN_MANDATORY_LABEL Label;
    HANDLE Low = NULL;
    PSID LowSid = NULL;

    if (!CreateRestrictedToken(Token, 0, 0, NULL, 0, NULL, 0, NULL, &Low))
        return NULL;
    if (!AllocateAndInitializeSid(&LabelAuthority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &LowSid))
    {
        CloseHandle(Low);
        return NULL;
    }
    Label.Label.Sid = LowSid;
    Label.Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(Low, TokenIntegrityLevel, &Label, sizeof(Label)))
    {
        CloseHandle(Low);
        Low = NULL;
    }
    FreeSid(LowSid);
    return Low;
}

static
BOOL
SetPolicyDword(PCWSTR Name, DWORD Value, DWORD *Previous, BOOL *HadValue)
{
    HKEY hKey;
    DWORD Size = sizeof(*Previous), Type;
    LONG Result;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ELEV_POLICY_KEY, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return FALSE;
    *HadValue = (RegQueryValueExW(hKey, Name, NULL, &Type, (LPBYTE)Previous, &Size) == ERROR_SUCCESS && Type == REG_DWORD);
    Result = RegSetValueExW(hKey, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    RegCloseKey(hKey);
    return Result == ERROR_SUCCESS;
}

static
void
RestorePolicyDword(PCWSTR Name, DWORD Previous, BOOL HadValue)
{
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ELEV_POLICY_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    if (HadValue)
        RegSetValueExW(hKey, Name, 0, REG_DWORD, (const BYTE *)&Previous, sizeof(Previous));
    else
        RegDeleteValueW(hKey, Name);
    RegCloseKey(hKey);
}

/* A prompt that is open: the image stays open underneath it (its directory cannot be
 * renamed), a second request of the logon is busy, and closing the prompt is a refusal. */
static
void
TestOpenPrompt(HANDLE Lua)
{
    WCHAR Dir[MAX_PATH], Moved[MAX_PATH], Image[MAX_PATH], Source[MAX_PATH];
    SECURITY_ATTRIBUTES Readable = { sizeof(Readable), NULL, FALSE };
    HANDLE hChild = NULL;
    HWND hwndPrompt = NULL;
    DWORD ExitCode, Previous = 0, Tick;
    BOOL HadValue = FALSE, Ret;

    if (!GetWindowsDirectoryW(Dir, _countof(Dir)) || !GetSystemDirectoryW(Source, _countof(Source)))
        return;
    StringCchCatW(Dir, _countof(Dir), ELEV_IMAGE_DIR);
    StringCchPrintfW(Moved, _countof(Moved), L"%ls.moved", Dir);
    StringCchPrintfW(Image, _countof(Image), L"%ls\\cmd.exe", Dir);
    StringCchCatW(Source, _countof(Source), L"\\cmd.exe");
    /* Everyone reads and runs, so the limited caller can name the copy */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;GRGX;;;WD)(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)",
                                                              SDDL_REVISION_1, &Readable.lpSecurityDescriptor, NULL))
    {
        skip("Cannot build the image directory's descriptor: %lu\n", GetLastError());
        return;
    }
    RemoveDirectoryW(Moved);
    if (!CreateDirectoryW(Dir, &Readable) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        skip("Cannot create the image directory: %lu\n", GetLastError());
        goto Cleanup;
    }
    if (!CopyFileW(Source, Image, FALSE))
    {
        skip("Cannot copy the image: %lu\n", GetLastError());
        goto Cleanup;
    }
    if (!SetPolicyDword(L"ConsentPromptBehaviorAdmin", 2, &Previous, &HadValue))
    {
        skip("Cannot set the consent policy\n");
        goto Cleanup;
    }

    Ret = StartChildAsync(Lua, "callapp", NULL, Image, &hChild);
    ok(Ret, "Starting the prompting child failed: %lu\n", GetLastError());
    if (!Ret)
        goto Restore;
    for (Tick = 0; Tick < 300 && hwndPrompt == NULL; Tick++)
    {
        hwndPrompt = FindWindowW(NULL, ELEV_CONSENT_TITLE);
        if (hwndPrompt == NULL)
        {
            if (WaitForSingleObject(hChild, 0) == WAIT_OBJECT_0)
                break;
            Sleep(100);
        }
    }
    if (hwndPrompt == NULL)
    {
        WaitChild(hChild, 1000, &ExitCode);
        ok(FALSE, "No consent prompt appeared (child exit %lu)\n", ExitCode);
        goto Restore;
    }

    Ret = MoveFileW(Dir, Moved);
    ok(!Ret, "The image directory could be renamed while the prompt is open\n");
    if (Ret)
        MoveFileW(Moved, Dir);

    Ret = StartChild(Lua, "call", NULL, &ExitCode);
    ok(Ret && ExitCode == ERROR_BUSY, "Second request during the prompt: ret %d, error %lu\n", Ret, ExitCode);

    PostMessageW(hwndPrompt, WM_CLOSE, 0, 0);
    WaitChild(hChild, 30000, &ExitCode);
    ok(ExitCode == ERROR_CANCELLED, "Closed prompt: expected ERROR_CANCELLED, got %lu\n", ExitCode);

    Ret = MoveFileW(Dir, Moved);
    ok(Ret, "The image directory cannot be renamed after the prompt: %lu\n", GetLastError());
    if (Ret)
        MoveFileW(Moved, Dir);

Restore:
    RestorePolicyDword(L"ConsentPromptBehaviorAdmin", Previous, HadValue);
Cleanup:
    DeleteFileW(Image);
    RemoveDirectoryW(Dir);
    LocalFree(Readable.lpSecurityDescriptor);
}

/* SeTcb + a local user: a real pair (LUA token of one logon linked to the full token of
 * another), policy 0 so no dialog is needed, and the elevated start itself. */
static
void
TestPairedCaller(void)
{
    USER_INFO_1 User = { 0 };
    LOCALGROUP_MEMBERS_INFO_3 Member;
    NET_API_STATUS NetStatus;
    HANDLE T1 = NULL, T2 = NULL, Lua2 = NULL, Imp = NULL;
    TOKEN_LINKED_TOKEN Link;
    DWORD ExitCode, Previous = 0, Value;
    BOOL HadValue = FALSE, Ret;

    NetUserDel(NULL, ELEV_TEST_USER);
    User.usri1_name = ELEV_TEST_USER;
    User.usri1_password = ELEV_TEST_PASSWORD;
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    if (NetUserAdd(NULL, 1, (LPBYTE)&User, NULL) != NERR_Success)
    {
        skip("The paired-caller checks need a local user\n");
        return;
    }
    /* The elevated side must really be an administrator */
    Member.lgrmi3_domainandname = ELEV_TEST_USER;
    NetStatus = NetLocalGroupAddMembers(NULL, L"Administrators", 3, (LPBYTE)&Member, 1);
    if (NetStatus != NERR_Success)
    {
        skip("Cannot make the test user an administrator: %lu\n", NetStatus);
        goto Cleanup;
    }

    Ret = LogonUserW(ELEV_TEST_USER, L".", ELEV_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T1) &&
          LogonUserW(ELEV_TEST_USER, L".", ELEV_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T2);
    ok(Ret, "Logging the test user on failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;
    if (GetTokenDword(T2, TokenElevationType, &Value) && Value == TokenElevationTypeLimited)
    {
        /* EnableLUA=1: the logon already came as a pair; its limited side is the caller */
        trace("The logon is split already; using the pair LSA built\n");
        Lua2 = T2;
        T2 = NULL;
    }
    else
    {
        Ret = CreateRestrictedToken(T2, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &Lua2);
        ok(Ret, "CreateRestrictedToken(LUA_TOKEN) failed: %lu\n", GetLastError());
        if (!Ret)
            goto Cleanup;
        Link.LinkedToken = T1;
        Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
        ok(Ret, "Linking the pair failed: %lu\n", GetLastError());
        if (!Ret)
            goto Cleanup;
    }

    if (!SetPolicyDword(L"ConsentPromptBehaviorAdmin", 0, &Previous, &HadValue))
    {
        skip("Cannot set the lab consent policy\n");
        goto Cleanup;
    }

    /* The limited side gets its program started with the elevated token */
    Ret = StartChild(Lua2, "elev", NULL, &ExitCode);
    ok(Ret, "Starting the limited child failed: %lu\n", GetLastError());
    ok(Ret && ExitCode == 0, "Elevated start failures: 0x%lx (1 no handle, 2 thread handle, 4 WriteProcessMemory allowed, "
       "8 not elevated, 10 token not readable, 20 no exit, 40 caller environment visible, 80 subst image accepted, "
       "100 control run blind to the variable, 10000000+err broker error)\n", ExitCode);

    /* A thread impersonating the other logon of the same user is refused */
    if (DuplicateTokenEx(T1, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &Imp))
    {
        Ret = StartChild(Lua2, "imp", Imp, &ExitCode);
        ok(Ret && (ExitCode == ERROR_ACCESS_DENIED || ExitCode == ELEV_CHILD_PIPE_DENIED),
           "Impersonating caller: ret %d, error %lu\n", Ret, ExitCode);
    }

    RestorePolicyDword(L"ConsentPromptBehaviorAdmin", Previous, HadValue);

    TestOpenPrompt(Lua2);

Cleanup:
    if (Imp) CloseHandle(Imp);
    if (Lua2) CloseHandle(Lua2);
    if (T2) CloseHandle(T2);
    if (T1) CloseHandle(T1);
    NetUserDel(NULL, ELEV_TEST_USER);
}

START_TEST(Elevation)
{
    char **Argv;
    int Argc = winetest_get_mainargs(&Argv);
    SECL_RESPONSE Response;
    TOKEN_PRIVILEGES Enable;
    SID_IDENTIFIER_AUTHORITY WorldAuthority = { SECURITY_WORLD_SID_AUTHORITY };
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    SID_AND_ATTRIBUTES Restricting[4] = { { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 } };
    PSID InteractiveSid = NULL;
    BYTE UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE], GroupBuffer[4096];
    DWORD RestrictingCount = 0, Length, i;
    HANDLE Process = NULL, Low = NULL, Restricted = NULL;
    DWORD dwError, Value, ExitCode;
    BOOL Ret;

    if (Argc >= 5 && !strcmp(Argv[2], "child"))
    {
        WCHAR Extra[MAX_PATH] = L"";

        if (Argc >= 6)
            MultiByteToWideChar(CP_ACP, 0, Argv[5], -1, Extra, _countof(Extra));
        ExitProcess(RunChild(Argv[3], (HANDLE)(ULONG_PTR)strtoull(Argv[4], NULL, 16), Extra));
    }

    /* Our own call: a non-limited caller is sent to the credentials path */
    dwError = CallBroker(L"cmd.exe /c exit 0", NULL, &Response);
    if (dwError == ERROR_SERVICE_NOT_ACTIVE)
    {
        skip("The secondary logon service is not running\n");
        return;
    }
    if (Response.hProcess != 0)
        CloseHandle((HANDLE)Response.hProcess);
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Process), "OpenProcessToken failed: %lu\n", GetLastError());
    if (!Process)
        return;
    if (GetTokenDword(Process, TokenElevationType, &Value) && Value == TokenElevationTypeLimited)
        trace("The runner itself is limited: broker answered %lu\n", dwError);
    else
        ok(dwError == ERROR_ELEVATION_REQUIRED || dwError == ERROR_ACCESS_DENIED,
           "Unpaired caller: expected ERROR_ELEVATION_REQUIRED, got %lu\n", dwError);

    /* Low integrity and restricted callers never reach a prompt. A Low caller may already be
     * refused at the pipe (mandatory policy); the child then reports where it was stopped. */
    Low = MakeLowToken(Process);
    if (Low)
    {
        Ret = StartChild(Low, "call", NULL, &ExitCode);
        ok(Ret && (ExitCode == ERROR_ACCESS_DENIED || ExitCode == ELEV_CHILD_PIPE_DENIED),
           "Low caller: ret %d, error %lu\n", Ret, ExitCode);
        trace("Low caller refused %s\n", ExitCode == ELEV_CHILD_PIPE_DENIED ? "at the pipe" : "by the broker");
        CloseHandle(Low);
    }
    else
        skip("Cannot build a Low token: %lu\n", GetLastError());
    /* A restricted token has restricting SIDs; WRITE_RESTRICTED alone is not one. The user,
     * the logon SID and Everyone stay so the restricted child can still start at all. */
    if (GetTokenInformation(Process, TokenUser, UserBuffer, sizeof(UserBuffer), &Length))
        Restricting[RestrictingCount++].Sid = ((PTOKEN_USER)UserBuffer)->User.Sid;
    if (GetTokenInformation(Process, TokenGroups, GroupBuffer, sizeof(GroupBuffer), &Length))
    {
        PTOKEN_GROUPS Groups = (PTOKEN_GROUPS)GroupBuffer;
        for (i = 0; i < Groups->GroupCount; i++)
        {
            if ((Groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
                Restricting[RestrictingCount++].Sid = Groups->Groups[i].Sid;
        }
    }
    if (AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_INTERACTIVE_RID, 0, 0, 0, 0, 0, 0, 0, &InteractiveSid))
        Restricting[RestrictingCount++].Sid = InteractiveSid;
    if (AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &Restricting[RestrictingCount].Sid))
    {
        RestrictingCount++;
        if (CreateRestrictedToken(Process, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, RestrictingCount, Restricting, &Restricted))
        {
            Ret = StartChild(Restricted, "call", NULL, &ExitCode);
            ok(Ret && ExitCode == ERROR_ACCESS_DENIED, "Restricted caller: ret %d, error %lu\n", Ret, ExitCode);
            CloseHandle(Restricted);
        }
        FreeSid(Restricting[RestrictingCount - 1].Sid);
    }
    if (InteractiveSid)
        FreeSid(InteractiveSid);

    /* The elevated start itself needs SeTcbPrivilege and a local user */
    Enable.PrivilegeCount = 1;
    Enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, L"SeTcbPrivilege", &Enable.Privileges[0].Luid);
    AdjustTokenPrivileges(Process, FALSE, &Enable, 0, NULL, NULL);
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        skip("SeTcbPrivilege is not held; the paired-caller checks need to run as SYSTEM\n");
    else
        TestPairedCaller();

    CloseHandle(Process);
}

