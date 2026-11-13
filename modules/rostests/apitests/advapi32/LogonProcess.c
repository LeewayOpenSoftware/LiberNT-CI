/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for CreateProcessWithLogonW with a local account
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <winbase.h>
#include <lm.h>

#define TEST_USER L"LntRunAsUser"
#define TEST_PASSWORD L"Lnt!RunAs#2026q"
#define TEST_EXIT_CODE 7

static
NET_API_STATUS
AddUser(VOID)
{
    USER_INFO_1 Info = { 0 };

    NetUserDel(NULL, TEST_USER);
    Info.usri1_name = (PWSTR)TEST_USER;
    Info.usri1_password = (PWSTR)TEST_PASSWORD;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    return NetUserAdd(NULL, 1, (LPBYTE)&Info, NULL);
}

/* Runs "cmd.exe /c exit 7" through CreateProcessWithLogonW. Returns the API
 * result, its last error, and the exit code of the child when it started. */
static
BOOL
RunWithLogon(
    _In_ PCWSTR Password,
    _In_ DWORD LogonFlags,
    _Out_ PDWORD Error,
    _Out_ PDWORD ExitCode)
{
    WCHAR CommandLine[MAX_PATH + 32];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo = { 0 };
    BOOL Result;

    *ExitCode = 0xdeadbeef;
    GetSystemDirectoryW(CommandLine, MAX_PATH);
    StringCchCatW(CommandLine, _countof(CommandLine), L"\\cmd.exe /c exit 7");

    SetLastError(0xdeadbeef);
    Result = CreateProcessWithLogonW(TEST_USER, L".", Password, LogonFlags, NULL, CommandLine,
                                     CREATE_NO_WINDOW, NULL, NULL, &StartupInfo, &ProcessInfo);
    *Error = GetLastError();
    if (Result)
    {
        WaitForSingleObject(ProcessInfo.hProcess, 30000);
        GetExitCodeProcess(ProcessInfo.hProcess, ExitCode);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
    trace("flags 0x%lx: result %d error %lu exit code 0x%lx\n", LogonFlags, Result, *Error, *ExitCode);
    return Result;
}

START_TEST(LogonProcess)
{
    NET_API_STATUS Status;
    HANDLE Token;
    DWORD Error, ExitCode;
    BOOL Result;

    Status = AddUser();
    if (Status != NERR_Success)
    {
        skip("NetUserAdd failed with %lu\n", Status);
        return;
    }

    /* The interactive logon that CreateProcessWithLogonW must perform */
    SetLastError(0xdeadbeef);
    if (!LogonUserW(TEST_USER, L".", TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &Token))
    {
        skip("Interactive logon failed with %lu\n", GetLastError());
        NetUserDel(NULL, TEST_USER);
        return;
    }
    CloseHandle(Token);

    /* Default logon: must not fail with ERROR_INVALID_PARAMETER (logon type 0) */
    Result = RunWithLogon(TEST_PASSWORD, 0, &Error, &ExitCode);
    ok(Result, "CreateProcessWithLogonW failed, error %lu\n", Error);
    ok(Error != ERROR_INVALID_PARAMETER, "Error is ERROR_INVALID_PARAMETER\n");
    if (Result)
        ok(ExitCode == TEST_EXIT_CODE, "Exit code 0x%lx, expected %d\n", ExitCode, TEST_EXIT_CODE);

    Result = RunWithLogon(TEST_PASSWORD, LOGON_WITH_PROFILE, &Error, &ExitCode);
    ok(Result, "CreateProcessWithLogonW(LOGON_WITH_PROFILE) failed, error %lu\n", Error);
    if (Result)
        ok(ExitCode == TEST_EXIT_CODE, "Exit code 0x%lx, expected %d\n", ExitCode, TEST_EXIT_CODE);

    /* A wrong password is a logon failure, not a parameter error */
    Result = RunWithLogon(L"wrong", 0, &Error, &ExitCode);
    ok(!Result, "CreateProcessWithLogonW succeeded with a wrong password\n");
    ok(Error == ERROR_LOGON_FAILURE, "Error %lu, expected ERROR_LOGON_FAILURE\n", Error);

    /* Network credentials only, with the account password: the process must still start */
    Result = RunWithLogon(TEST_PASSWORD, LOGON_NETCREDENTIALS_ONLY, &Error, &ExitCode);
    ok(Result, "CreateProcessWithLogonW(LOGON_NETCREDENTIALS_ONLY) failed, error %lu\n", Error);
    if (Result)
        ok(ExitCode == TEST_EXIT_CODE, "Exit code 0x%lx, expected %d\n", ExitCode, TEST_EXIT_CODE);

    NetUserDel(NULL, TEST_USER);
}
