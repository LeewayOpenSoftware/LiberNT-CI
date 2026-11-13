/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The secondary logon service serves only the caller's own process, in the caller's own session,
 *              and gives the new logon only a desktop the caller may use itself
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 *              Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <lm.h>
#include <sddl.h>
#include <winuser.h>
#include <seclogon_c.h>

#define TEST_USER L"LntSeclCallerTest"
#define TEST_PASSWORD L"Lnt!SeclCaller#2026q"

void __RPC_FAR * __RPC_USER midl_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

void __RPC_USER midl_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

static WCHAR ChildCommandLine[MAX_PATH + 32];

static
handle_t
BindSeclogon(VOID)
{
    RPC_WSTR StringBinding = NULL;
    handle_t hBinding = NULL;

    if (RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncacn_np", NULL, (RPC_WSTR)L"\\pipe\\seclogon", NULL,
                                 &StringBinding) != RPC_S_OK)
    {
        return NULL;
    }

    if (RpcBindingFromStringBindingW(StringBinding, &hBinding) != RPC_S_OK)
        hBinding = NULL;

    RpcStringFreeW(&StringBinding);
    return hBinding;
}

static
VOID
StartSeclogon(VOID)
{
    SC_HANDLE hManager, hService;

    hManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hManager == NULL)
        return;

    hService = OpenServiceW(hManager, L"seclogon", SERVICE_START);
    if (hService != NULL)
    {
        StartServiceW(hService, 0, NULL);
        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hManager);
}

/* Asks the service for a suspended child of the test user on behalf of the process named in the request */
static
DWORD
RequestChildEx(
    _In_ DWORD dwNamedProcessId,
    _In_ DWORD dwLogonFlags,
    _In_opt_ PWSTR Desktop,
    _Out_ PDWORD pdwChildProcessId)
{
    SECL_REQUEST Request;
    SECL_RESPONSE Response;
    handle_t hBinding;
    DWORD dwError;

    *pdwChildProcessId = 0;

    hBinding = BindSeclogon();
    if (hBinding == NULL)
        return ERROR_SERVICE_NOT_ACTIVE;

    ZeroMemory(&Request, sizeof(Request));
    ZeroMemory(&Response, sizeof(Response));
    Request.Username = TEST_USER;
    Request.Domain = L".";
    Request.Password = TEST_PASSWORD;
    Request.CommandLine = ChildCommandLine;
    Request.dwCreationFlags = CREATE_SUSPENDED;
    Request.dwProcessId = dwNamedProcessId;
    Request.dwLogonFlags = dwLogonFlags;
    Request.Desktop = Desktop;
    Request.dwDesktopInherited = (Desktop != NULL);

    RpcTryExcept
    {
        SeclCreateProcessWithLogonW(hBinding, &Request, &Response);
        dwError = Response.dwError;
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        dwError = RpcExceptionCode();
    }
    RpcEndExcept;

    RpcBindingFree(&hBinding);

    if (dwError == ERROR_SUCCESS)
        *pdwChildProcessId = Response.dwProcessId;

    return dwError;
}

static
DWORD
RequestChild(
    _In_ DWORD dwNamedProcessId,
    _Out_ PDWORD pdwChildProcessId)
{
    return RequestChildEx(dwNamedProcessId, 0, NULL, pdwChildProcessId);
}

static
VOID
EndChild(
    _In_ DWORD dwProcessId)
{
    HANDLE hProcess;

    if (dwProcessId == 0)
        return;

    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, dwProcessId);
    if (hProcess != NULL)
    {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
    }
}

static
BOOL
SetOwnPrivilege(
    _In_ PCWSTR Name,
    _In_ BOOL Enable,
    _Out_ PBOOL WasEnabled)
{
    TOKEN_PRIVILEGES New, Old;
    DWORD Length = sizeof(Old);
    HANDLE hToken;
    BOOL Result;

    *WasEnabled = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    New.PrivilegeCount = 1;
    New.Privileges[0].Attributes = Enable ? SE_PRIVILEGE_ENABLED : 0;
    Result = LookupPrivilegeValueW(NULL, Name, &New.Privileges[0].Luid) &&
             AdjustTokenPrivileges(hToken, FALSE, &New, sizeof(Old), &Old, &Length) &&
             GetLastError() == ERROR_SUCCESS;
    if (Result && Old.PrivilegeCount == 1)
        *WasEnabled = (Old.Privileges[0].Attributes & SE_PRIVILEGE_ENABLED) != 0;

    CloseHandle(hToken);
    return Result;
}

/* A process the caller cannot open for handles may not be named as the caller */
static
VOID
TestForeignProcess(VOID)
{
    STARTUPINFOW StartupInfo = {sizeof(StartupInfo)};
    PROCESS_INFORMATION Victim = {0};
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes)};
    PSECURITY_DESCRIPTOR pSD = NULL;
    BOOL WasEnabled = FALSE, Adjusted;
    DWORD dwChild, dwError;
    HANDLE hProbe;

    ok(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;0x101000;;;WD)", SDDL_REVISION_1, &pSD, NULL),
       "ConvertStringSecurityDescriptorToSecurityDescriptorW failed %lu\n", GetLastError());
    Attributes.lpSecurityDescriptor = pSD;

    if (!CreateProcessW(NULL, ChildCommandLine, &Attributes, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &Victim))
    {
        ok(FALSE, "CreateProcessW failed %lu\n", GetLastError());
        LocalFree(pSD);
        return;
    }

    /* The debug privilege would open any process */
    Adjusted = SetOwnPrivilege(L"SeDebugPrivilege", FALSE, &WasEnabled);

    hProbe = OpenProcess(PROCESS_DUP_HANDLE, FALSE, Victim.dwProcessId);
    if (hProbe != NULL)
    {
        skip("The caller can open the other process for handles\n");
        CloseHandle(hProbe);
    }
    else
    {
        dwError = RequestChild(Victim.dwProcessId, &dwChild);
        ok(dwError == ERROR_ACCESS_DENIED, "a request naming another process: error %lu\n", dwError);
        EndChild(dwChild);
    }

    if (Adjusted && WasEnabled)
        SetOwnPrivilege(L"SeDebugPrivilege", TRUE, &WasEnabled);

    TerminateProcess(Victim.hProcess, 0);
    CloseHandle(Victim.hThread);
    CloseHandle(Victim.hProcess);
    LocalFree(pSD);
}

/* The child goes to the caller's session, whatever session the named process runs in */
static
VOID
TestOtherSession(VOID)
{
    STARTUPINFOW StartupInfo = {sizeof(StartupInfo)};
    PROCESS_INFORMATION Victim = {0};
    DWORD OwnSession = MAXDWORD, OtherSession, ChildSession = MAXDWORD;
    HANDLE hToken = NULL, hOtherToken = NULL;
    BOOL WasEnabled;
    DWORD dwChild, dwError;

    OtherSession = WTSGetActiveConsoleSessionId();
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &OwnSession) || OtherSession == MAXDWORD ||
        OtherSession == OwnSession)
    {
        skip("No other session to start a process in\n");
        return;
    }

    /* Placing a process in another session needs the TCB privilege */
    if (!SetOwnPrivilege(L"SeTcbPrivilege", TRUE, &WasEnabled) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hToken) ||
        !DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hOtherToken) ||
        !SetTokenInformation(hOtherToken, TokenSessionId, &OtherSession, sizeof(OtherSession)))
    {
        skip("A process cannot be placed in session %lu here: %lu\n", OtherSession, GetLastError());
        if (hOtherToken != NULL)
            CloseHandle(hOtherToken);
        if (hToken != NULL)
            CloseHandle(hToken);
        return;
    }

    if (!CreateProcessAsUserW(hOtherToken, NULL, ChildCommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                              &StartupInfo, &Victim))
    {
        ok(FALSE, "CreateProcessAsUserW in session %lu failed %lu\n", OtherSession, GetLastError());
        CloseHandle(hOtherToken);
        CloseHandle(hToken);
        return;
    }

    dwError = RequestChild(Victim.dwProcessId, &dwChild);
    ok(dwError == ERROR_SUCCESS, "a request naming a process of session %lu: error %lu\n", OtherSession, dwError);
    if (dwError == ERROR_SUCCESS)
    {
        ok(ProcessIdToSessionId(dwChild, &ChildSession), "ProcessIdToSessionId failed %lu\n", GetLastError());
        ok(ChildSession == OwnSession, "the child runs in session %lu, the caller in %lu\n", ChildSession, OwnSession);
        EndChild(dwChild);
    }

    TerminateProcess(Victim.hProcess, 0);
    CloseHandle(Victim.hThread);
    CloseHandle(Victim.hProcess);
    CloseHandle(hOtherToken);
    CloseHandle(hToken);

    if (!WasEnabled)
        SetOwnPrivilege(L"SeTcbPrivilege", FALSE, &WasEnabled);
}

static
BOOL
CanOpenDesktop(
    _In_ PCWSTR Name)
{
    HDESK hDesktop = OpenDesktopW(Name, 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW);

    if (hDesktop == NULL)
        return FALSE;
    CloseDesktop(hDesktop);
    return TRUE;
}

/* A desktop the caller cannot use is never opened to the new logon, here the caller's own (LOGON_NETCREDENTIALS_ONLY) */
static
VOID
TestDesktopGrant(VOID)
{
    WCHAR WinSta[64], Desktop[64], Path[160];
    DWORD dwChild, dwError;

    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, WinSta, sizeof(WinSta), NULL) ||
        _wcsicmp(WinSta, L"WinSta0") != 0 ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, Desktop, sizeof(Desktop), NULL))
    {
        skip("Not on the interactive window station\n");
        return;
    }

    if (CanOpenDesktop(L"Winlogon"))
    {
        skip("The caller can already open the Winlogon desktop\n");
    }
    else
    {
        dwError = RequestChildEx(GetCurrentProcessId(), LOGON_NETCREDENTIALS_ONLY, L"WinSta0\\Winlogon", &dwChild);
        trace("a request inheriting WinSta0\\Winlogon: error %lu\n", dwError);
        ok(!CanOpenDesktop(L"Winlogon"), "the request opened the Winlogon desktop to the caller's logon\n");
        EndChild(dwChild);
    }

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", WinSta, Desktop);
    dwError = RequestChildEx(GetCurrentProcessId(), LOGON_NETCREDENTIALS_ONLY, Path, &dwChild);
    ok(dwError == ERROR_SUCCESS, "a request inheriting the caller's own desktop %ls: error %lu\n", Path, dwError);
    EndChild(dwChild);
}

START_TEST(SeclogonCaller)
{
    USER_INFO_1 User = {0};
    NET_API_STATUS Status;
    char **argv;

    /* The child is only ever looked at, never run */
    if (winetest_get_mainargs(&argv) >= 3)
        return;

    GetModuleFileNameW(NULL, ChildCommandLine, MAX_PATH);
    StringCchCatW(ChildCommandLine, ARRAYSIZE(ChildCommandLine), L" SeclogonCaller child");

    NetUserDel(NULL, TEST_USER);
    User.usri1_name = TEST_USER;
    User.usri1_password = TEST_PASSWORD;
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&User, NULL);
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("Creating a user needs administrator rights\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd failed %lu\n", Status);
    if (Status != NERR_Success)
        return;

    StartSeclogon();
    TestForeignProcess();
    TestOtherSession();
    TestDesktopGrant();

    NetUserDel(NULL, TEST_USER);
}
