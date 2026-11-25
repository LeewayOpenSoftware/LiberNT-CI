/*
 * PROJECT:     LiberNT netapi32.dll API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the must-change-password state (usri4_password_expired, net user /logonpasswordchg)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <lm.h>

#define TEST_USER L"LntMustChgTest"
#define TEST_PASSWORD L"Lnt!MustChg#2026q"

static
PUSER_INFO_4
GetInfo4(VOID)
{
    PUSER_INFO_4 Info = NULL;
    NET_API_STATUS Status;

    Status = NetUserGetInfo(NULL, TEST_USER, 4, (LPBYTE *)&Info);
    ok(Status == NERR_Success, "NetUserGetInfo returned %lu\n", Status);
    return (Status == NERR_Success) ? Info : NULL;
}

/* Read-modify-write at level 4, the way net.exe changes an account */
static
NET_API_STATUS
SetComment(PCWSTR Comment, DWORD PasswordExpired)
{
    PUSER_INFO_4 Info;
    NET_API_STATUS Status;

    Info = GetInfo4();
    if (Info == NULL)
        return NERR_InternalError;

    Info->usri4_comment = (PWSTR)Comment;
    if (PasswordExpired != (DWORD)-1)
        Info->usri4_password_expired = PasswordExpired;
    Status = NetUserSetInfo(NULL, TEST_USER, 4, (LPBYTE)Info, NULL);
    NetApiBufferFree(Info);
    return Status;
}

static
DWORD
TryLogon(VOID)
{
    HANDLE Token;

    if (LogonUserW(TEST_USER, L".", TEST_PASSWORD, LOGON32_LOGON_NETWORK,
                   LOGON32_PROVIDER_DEFAULT, &Token))
    {
        CloseHandle(Token);
        return ERROR_SUCCESS;
    }
    return GetLastError();
}

static
DWORD
RunNet(PCWSTR Arguments)
{
    WCHAR CommandLine[MAX_PATH * 2];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD ExitCode = (DWORD)-1;
    UINT Length;

    Length = GetSystemDirectoryW(CommandLine, MAX_PATH);
    if (Length == 0 || Length >= MAX_PATH)
        return ExitCode;
    _snwprintf(CommandLine + Length, _countof(CommandLine) - Length, L"\\net.exe %s", Arguments);
    CommandLine[_countof(CommandLine) - 1] = UNICODE_NULL;

    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
    {
        skip("CreateProcess(net.exe) failed with %lu\n", GetLastError());
        return ExitCode;
    }
    ok(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0, "net.exe did not finish\n");
    GetExitCodeProcess(pi.hProcess, &ExitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ExitCode;
}

static
VOID
ResetPassword(VOID)
{
    USER_INFO_1003 Info;
    NET_API_STATUS Status;

    Info.usri1003_password = (PWSTR)TEST_PASSWORD;
    Status = NetUserSetInfo(NULL, TEST_USER, 1003, (LPBYTE)&Info, NULL);
    ok(Status == NERR_Success, "Setting the password returned %lu\n", Status);
}

START_TEST(NetUserMustChange)
{
    USER_INFO_1 Add = { 0 };
    PUSER_MODALS_INFO_0 Modals = NULL;
    PUSER_INFO_4 Info;
    NET_API_STATUS Status;
    DWORD Age;

    Status = NetUserModalsGet(NULL, 0, (LPBYTE *)&Modals);
    ok(Status == NERR_Success, "NetUserModalsGet returned %lu\n", Status);
    if (Modals == NULL)
        return;
    if (Modals->usrmod0_max_passwd_age == TIMEQ_FOREVER)
    {
        skip("The maximum password age is unlimited\n");
        NetApiBufferFree(Modals);
        return;
    }
    NetApiBufferFree(Modals);

    NetUserDel(NULL, TEST_USER);
    Add.usri1_name = (PWSTR)TEST_USER;
    Add.usri1_password = (PWSTR)TEST_PASSWORD;
    Add.usri1_priv = USER_PRIV_USER;
    Add.usri1_flags = UF_SCRIPT;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Add, NULL);
    ok(Status == NERR_Success, "NetUserAdd returned %lu\n", Status);
    if (Status != NERR_Success)
        return;

    Info = GetInfo4();
    if (Info == NULL)
        goto cleanup;
    ok(Info->usri4_password_expired == 0, "A new account reads as expired\n");
    NetApiBufferFree(Info);
    ok(TryLogon() == ERROR_SUCCESS, "Logon of a new account failed\n");

    /* An unrelated change must not reset the password age */
    Sleep(3000);
    Status = SetComment(L"age", (DWORD)-1);
    ok(Status == NERR_Success, "NetUserSetInfo(comment) returned %lu\n", Status);
    Info = GetInfo4();
    if (Info == NULL)
        goto cleanup;
    Age = Info->usri4_password_age;
    ok(Age >= 2, "Changing the comment reset the password age to %lu\n", Age);
    NetApiBufferFree(Info);

    /* Set must-change; it has to read back and be enforced */
    Status = SetComment(L"expire", 1);
    ok(Status == NERR_Success, "NetUserSetInfo(password_expired=1) returned %lu\n", Status);
    Info = GetInfo4();
    if (Info == NULL)
        goto cleanup;
    ok(Info->usri4_password_expired != 0, "usri4_password_expired does not read back\n");
    NetApiBufferFree(Info);
    ok(TryLogon() == ERROR_PASSWORD_MUST_CHANGE, "Logon did not report must-change\n");

    /* A later read-modify-write keeps the state */
    Status = SetComment(L"keep", (DWORD)-1);
    ok(Status == NERR_Success, "NetUserSetInfo(comment) returned %lu\n", Status);
    ok(TryLogon() == ERROR_PASSWORD_MUST_CHANGE, "A comment change cleared must-change\n");

    /* Zero cannot clear an expiry */
    Status = SetComment(L"zero", 0);
    ok(Status == NERR_Success, "NetUserSetInfo(password_expired=0) returned %lu\n", Status);
    ok(TryLogon() == ERROR_PASSWORD_MUST_CHANGE, "password_expired=0 cleared must-change\n");

    /* A new password ends it */
    ResetPassword();
    ok(TryLogon() == ERROR_SUCCESS, "Logon after a password reset failed\n");

    /* net user /logonpasswordchg:yes */
    ok(RunNet(L"user " TEST_USER L" /logonpasswordchg:yes") == 0, "net user /logonpasswordchg:yes failed\n");
    Info = GetInfo4();
    if (Info == NULL)
        goto cleanup;
    ok(Info->usri4_password_expired != 0, "net user /logonpasswordchg:yes did not set the state\n");
    NetApiBufferFree(Info);
    ok(TryLogon() == ERROR_PASSWORD_MUST_CHANGE, "Logon after net user /logonpasswordchg:yes did not report must-change\n");

    ok(RunNet(L"user " TEST_USER L" /logonpasswordchg:maybe") != 0, "net user accepted /logonpasswordchg:maybe\n");

cleanup:
    Status = NetUserDel(NULL, TEST_USER);
    ok(Status == NERR_Success, "NetUserDel returned %lu\n", Status);
}
