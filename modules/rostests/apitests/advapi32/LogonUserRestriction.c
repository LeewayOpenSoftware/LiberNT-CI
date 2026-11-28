/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     LogonUserW reports the specific account restriction in the Windows decision order
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>
#include <lm.h>

#define TEST_USER L"LntRestrict"
#define TEST_BLANK_USER L"LntRestrictBlank"
#define TEST_PASSWORD L"Lnt!Restrict#2026q"
#define LSA_KEY L"SYSTEM\\CurrentControlSet\\Control\\Lsa"
#define LIMIT_BLANK L"LimitBlankPasswordUse"

static
DWORD
Logon(
    _In_ PCWSTR Name,
    _In_ PCWSTR Password,
    _In_ DWORD Type)
{
    HANDLE Token = NULL;
    BOOL Result;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Result = LogonUserW(Name, L".", Password, Type, LOGON32_PROVIDER_DEFAULT, &Token);
    Error = Result ? ERROR_SUCCESS : GetLastError();
    if (Token)
        CloseHandle(Token);
    trace("%ls type %lu: result %d error %lu\n", Name, Type, Result, Error);
    return Error;
}

static
NET_API_STATUS
AddUser(
    _In_ PCWSTR Name,
    _In_ PCWSTR Password,
    _In_ BOOL Disabled)
{
    USER_INFO_1 Info = { 0 };

    NetUserDel(NULL, Name);
    Info.usri1_name = (PWSTR)Name;
    Info.usri1_password = (PWSTR)Password;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD |
                       (Password[0] ? 0 : UF_PASSWD_NOTREQD) |
                       (Disabled ? UF_ACCOUNTDISABLE : 0);
    return NetUserAdd(NULL, 1, (LPBYTE)&Info, NULL);
}

static
NET_API_STATUS
SetDisabled(
    _In_ PCWSTR Name,
    _In_ BOOL Blank,
    _In_ BOOL Disabled)
{
    USER_INFO_1008 Info;

    Info.usri1008_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD |
                          (Blank ? UF_PASSWD_NOTREQD : 0) |
                          (Disabled ? UF_ACCOUNTDISABLE : 0);
    return NetUserSetInfo(NULL, Name, 1008, (LPBYTE)&Info, NULL);
}

static
VOID
TestBlankPolicyOff(VOID)
{
    HKEY Key;
    DWORD Saved = 1, Size = sizeof(Saved), Type, Zero = 0;
    BOOL HadValue;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LSA_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
    {
        skip("Cannot open the Lsa key for writing\n");
        return;
    }
    HadValue = RegQueryValueExW(Key, LIMIT_BLANK, NULL, &Type, (LPBYTE)&Saved, &Size) == ERROR_SUCCESS &&
               Type == REG_DWORD;
    ok(RegSetValueExW(Key, LIMIT_BLANK, 0, REG_DWORD, (LPBYTE)&Zero, sizeof(Zero)) == ERROR_SUCCESS,
       "Setting %ls failed\n", LIMIT_BLANK);

    /* With the policy off the disabled flag still decides, and an enabled blank account logs on */
    ok(SetDisabled(TEST_BLANK_USER, TRUE, TRUE) == NERR_Success, "NetUserSetInfo failed\n");
    ok(Logon(TEST_BLANK_USER, L"", LOGON32_LOGON_INTERACTIVE) == ERROR_ACCOUNT_DISABLED, "policy off, disabled blank\n");
    ok(SetDisabled(TEST_BLANK_USER, TRUE, FALSE) == NERR_Success, "NetUserSetInfo failed\n");
    ok(Logon(TEST_BLANK_USER, L"", LOGON32_LOGON_INTERACTIVE) == ERROR_SUCCESS, "policy off, enabled blank\n");

    if (HadValue)
        RegSetValueExW(Key, LIMIT_BLANK, 0, REG_DWORD, (LPBYTE)&Saved, sizeof(Saved));
    else
        RegDeleteValueW(Key, LIMIT_BLANK);
    RegCloseKey(Key);
}

START_TEST(LogonUserRestriction)
{
    NET_API_STATUS Status;

    Status = AddUser(TEST_USER, TEST_PASSWORD, TRUE);
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("Creating a user needs administrator rights\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd failed %lu\n", Status);
    Status = AddUser(TEST_BLANK_USER, L"", TRUE);
    ok(Status == NERR_Success, "NetUserAdd for the blank password failed %lu\n", Status);

    /* A disabled account is ERROR_ACCOUNT_DISABLED, not the generic ERROR_ACCOUNT_RESTRICTION */
    ok(Logon(TEST_USER, TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE) == ERROR_ACCOUNT_DISABLED, "disabled\n");
    ok(Logon(TEST_USER, TEST_PASSWORD, LOGON32_LOGON_NETWORK) == ERROR_ACCOUNT_DISABLED, "disabled, network\n");

    /* A wrong password never reveals the account state */
    ok(Logon(TEST_USER, L"wrong", LOGON32_LOGON_INTERACTIVE) == ERROR_LOGON_FAILURE, "disabled, wrong password\n");
    ok(Logon(TEST_BLANK_USER, L"x", LOGON32_LOGON_INTERACTIVE) == ERROR_LOGON_FAILURE, "disabled blank, wrong password\n");
    ok(Logon(TEST_BLANK_USER, L"x", LOGON32_LOGON_NETWORK) == ERROR_LOGON_FAILURE, "disabled blank, wrong password, network\n");

    /* The disabled flag is decided before the blank password policy */
    ok(Logon(TEST_BLANK_USER, L"", LOGON32_LOGON_INTERACTIVE) == ERROR_ACCOUNT_DISABLED, "disabled blank\n");
    ok(Logon(TEST_BLANK_USER, L"", LOGON32_LOGON_NETWORK) == ERROR_ACCOUNT_DISABLED, "disabled blank, network\n");

    /* An enabled blank account is refused by the default policy */
    ok(SetDisabled(TEST_BLANK_USER, TRUE, FALSE) == NERR_Success, "NetUserSetInfo failed\n");
    ok(Logon(TEST_BLANK_USER, L"", LOGON32_LOGON_INTERACTIVE) == ERROR_ACCOUNT_RESTRICTION, "enabled blank\n");

    TestBlankPolicyOff();

    /* Control: the same password works once the account is enabled */
    ok(SetDisabled(TEST_USER, FALSE, FALSE) == NERR_Success, "NetUserSetInfo failed\n");
    ok(Logon(TEST_USER, TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE) == ERROR_SUCCESS, "enabled\n");

    NetUserDel(NULL, TEST_BLANK_USER);
    NetUserDel(NULL, TEST_USER);
}
