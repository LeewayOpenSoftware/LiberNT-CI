/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LogonUserW reports the specific account restriction, as Windows does
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>
#include <lm.h>

#define TEST_USER L"LntRestrict"
#define TEST_PASSWORD L"Lnt!Restrict#2026q"

static
DWORD
InteractiveLogon(
    _In_ PCWSTR Password)
{
    HANDLE Token = NULL;
    BOOL Result;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Result = LogonUserW(TEST_USER, L".", Password, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &Token);
    Error = Result ? ERROR_SUCCESS : GetLastError();
    if (Token)
        CloseHandle(Token);
    trace("result %d error %lu\n", Result, Error);
    return Error;
}

static
NET_API_STATUS
EnableUser(VOID)
{
    USER_INFO_1008 Info;

    Info.usri1008_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    return NetUserSetInfo(NULL, TEST_USER, 1008, (LPBYTE)&Info, NULL);
}

START_TEST(LogonUserRestriction)
{
    USER_INFO_1 Info = { 0 };
    NET_API_STATUS Status;

    NetUserDel(NULL, TEST_USER);
    Info.usri1_name = TEST_USER;
    Info.usri1_password = TEST_PASSWORD;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD | UF_ACCOUNTDISABLE;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Info, NULL);
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("Creating a user needs administrator rights\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd failed %lu\n", Status);

    /* A disabled account is ERROR_ACCOUNT_DISABLED, not the generic ERROR_ACCOUNT_RESTRICTION */
    ok(InteractiveLogon(TEST_PASSWORD) == ERROR_ACCOUNT_DISABLED, "disabled account logon\n");

    /* The password is checked before the disabled flag */
    ok(InteractiveLogon(L"wrong") == ERROR_LOGON_FAILURE, "disabled account, wrong password\n");

    /* Control: the same password works once the account is enabled */
    Status = EnableUser();
    ok(Status == NERR_Success, "NetUserSetInfo failed %lu\n", Status);
    ok(InteractiveLogon(TEST_PASSWORD) == ERROR_SUCCESS, "enabled account logon\n");

    NetUserDel(NULL, TEST_USER);
}
