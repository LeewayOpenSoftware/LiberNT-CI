/*
 * PROJECT:     LiberNT API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the elevation broker of the secondary logon service
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <winuser.h>
#include <lm.h>
#include <seclogon_c.h>

#ifndef TokenElevationTypeLimited
#define TokenElevationTypeLimited 3
#endif

#define ELEV_TEST_USER     L"LntElevationTest"
#define ELEV_TEST_PASSWORD L"Lnt!Elevation#2026q"
#define ELEV_POLICY_KEY    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"

/* Child exit codes: the broker's error for the 'call' modes, a bit mask for 'elev' */
#define ELEV_CHILD_UNEXPECTED   0x10000000

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

    ZeroMemory(Response, sizeof(*Response));
    Request.ApplicationName = (WCHAR *)ApplicationName;
    Request.CommandLine = (WCHAR *)CommandLine;
    Request.dwShowCmd = SW_HIDE;
    Request.dwCreationFlags = CREATE_NO_WINDOW;
    Request.dwDesktopInherited = TRUE;

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

/* 'call': return the broker's answer for a harmless command.
 * 'elev': expect an elevated start and check what came back (bit mask).
 * 'imp': call while impersonating the inherited token of another logon. */
static
DWORD
RunChild(PCSTR Mode, HANDLE Inherited)
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
        return CallBroker(Cmd, NULL, &Response);

    if (!strcmp(Mode, "imp"))
    {
        if (!SetThreadToken(NULL, Inherited))
            return ELEV_CHILD_UNEXPECTED | GetLastError();
        dwError = CallBroker(Cmd, NULL, &Response);
        RevertToSelf();
        return dwError;
    }

    /* elev: a variable of ours must not reach the elevated side (cmd's 'set X' exits 1 when X is undefined) */
    SetEnvironmentVariableW(L"LNT_ELEVATION_TEST", L"1");
    dwError = CallBroker(Cmd, NULL, &Response);
    if (dwError != ERROR_SUCCESS)
        return ELEV_CHILD_UNEXPECTED | dwError;
    hProcess = (HANDLE)Response.hProcess;
    if (hProcess == NULL)
        return 0x1;
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
        if (dwError != ERROR_BAD_PATHNAME)
            Failures |= 0x80;
        if (Response.hProcess != 0)
            CloseHandle((HANDLE)Response.hProcess);
        DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"Q:", NULL);
    }
    return Failures;
}

static
BOOL
StartChild(HANDLE Token, PCSTR Mode, HANDLE Inherited, DWORD *ExitCode)
{
    SECURITY_ATTRIBUTES Inheritable = { sizeof(Inheritable), NULL, TRUE };
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR Exe[MAX_PATH], CommandLine[MAX_PATH + 64];
    HANDLE Dup = NULL;
    BOOL Ret;

    *ExitCode = 0xffffffff;
    if (!GetModuleFileNameW(NULL, Exe, _countof(Exe)))
        return FALSE;
    if (Inherited != NULL &&
        !DuplicateHandle(GetCurrentProcess(), Inherited, GetCurrentProcess(), &Dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return FALSE;
    StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" Elevation child %hs %p", Exe, Mode, Dup);
    Ret = CreateProcessAsUserW(Token, Exe, CommandLine, NULL, NULL, Dup != NULL, CREATE_NO_WINDOW, NULL, NULL,
                               &StartupInfo, &ProcessInfo);
    if (Ret)
    {
        if (WaitForSingleObject(ProcessInfo.hProcess, 60000) == WAIT_OBJECT_0)
            GetExitCodeProcess(ProcessInfo.hProcess, ExitCode);
        else
            TerminateProcess(ProcessInfo.hProcess, 0);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
    if (Dup != NULL)
        CloseHandle(Dup);
    if (Inheritable.bInheritHandle) {}
    return Ret;
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

/* SeTcb + a local user: a real pair (LUA token of one logon linked to the full token of
 * another), policy 0 so no dialog is needed, and the elevated start itself. */
static
void
TestPairedCaller(void)
{
    USER_INFO_1 User = { 0 };
    LOCALGROUP_MEMBERS_INFO_0 Member;
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    HANDLE T1 = NULL, T2 = NULL, Lua2 = NULL, Imp = NULL;
    TOKEN_LINKED_TOKEN Link;
    PSID AdminsSid = NULL;
    DWORD ExitCode, Previous = 0;
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
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &AdminsSid))
    {
        Member.lgrmi0_sid = AdminsSid;
        NetLocalGroupAddMembers(NULL, L"Administrators", 0, (LPBYTE)&Member, 1);
        FreeSid(AdminsSid);
    }

    Ret = LogonUserW(ELEV_TEST_USER, L".", ELEV_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T1) &&
          LogonUserW(ELEV_TEST_USER, L".", ELEV_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T2) &&
          CreateRestrictedToken(T2, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &Lua2);
    ok(Ret, "Building the pair's tokens failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;
    Link.LinkedToken = T1;
    Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
    ok(Ret, "Linking the pair failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

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
       "10000000+err broker error)\n", ExitCode);

    /* A thread impersonating the other logon of the same user is refused */
    if (DuplicateTokenEx(T1, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &Imp))
    {
        Ret = StartChild(Lua2, "imp", Imp, &ExitCode);
        ok(Ret && ExitCode == ERROR_ACCESS_DENIED, "Impersonating caller: ret %d, error %lu\n", Ret, ExitCode);
    }

    RestorePolicyDword(L"ConsentPromptBehaviorAdmin", Previous, HadValue);

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
    HANDLE Process = NULL, Low = NULL, Restricted = NULL;
    DWORD dwError, Value, ExitCode;
    BOOL Ret;

    if (Argc >= 5 && !strcmp(Argv[2], "child"))
        ExitProcess(RunChild(Argv[3], (HANDLE)(ULONG_PTR)strtoull(Argv[4], NULL, 16)));

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

    /* Low integrity and restricted callers never reach a prompt */
    Low = MakeLowToken(Process);
    if (Low)
    {
        Ret = StartChild(Low, "call", NULL, &ExitCode);
        ok(Ret && ExitCode == ERROR_ACCESS_DENIED, "Low caller: ret %d, error %lu\n", Ret, ExitCode);
        CloseHandle(Low);
    }
    else
        skip("Cannot build a Low token: %lu\n", GetLastError());
    if (CreateRestrictedToken(Process, DISABLE_MAX_PRIVILEGE | WRITE_RESTRICTED, 0, NULL, 0, NULL, 0, NULL, &Restricted))
    {
        Ret = StartChild(Restricted, "call", NULL, &ExitCode);
        ok(Ret && ExitCode == ERROR_ACCESS_DENIED, "Restricted caller: ret %d, error %lu\n", Ret, ExitCode);
        CloseHandle(Restricted);
    }

    /* The elevated start itself needs SeTcbPrivilege and a local user */
    Enable.PrivilegeCount = 1;
    Enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, SE_TCB_NAME, &Enable.Privileges[0].Luid);
    AdjustTokenPrivileges(Process, FALSE, &Enable, 0, NULL, NULL);
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        skip("SeTcbPrivilege is not held; the paired-caller checks need to run as SYSTEM\n");
    else
        TestPairedCaller();

    CloseHandle(Process);
}

