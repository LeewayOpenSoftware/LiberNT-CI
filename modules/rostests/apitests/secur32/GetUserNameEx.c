/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests GetUserNameExW(NameSamCompatible)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#define SECURITY_WIN32
#include <security.h>
#include <lmcons.h>

/* Expected result: "<domain>\<account>" from LookupAccountSid of the token
 * user, i.e. the real domain (NT AUTHORITY for service accounts), not the
 * machine name. */
static BOOL
GetTokenUserSamName(LPWSTR Expected, SIZE_T cchExpected)
{
    HANDLE Token = NULL;
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD Length = 0;
    WCHAR Account[UNLEN + 1], Domain[256];
    DWORD cchAccount = ARRAYSIZE(Account), cchDomain = ARRAYSIZE(Domain);
    SID_NAME_USE Use;
    BOOL Ok = FALSE;

    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token) &&
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;

    if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length) &&
        LookupAccountSidW(NULL, ((PTOKEN_USER)Buffer)->User.Sid,
                          Account, &cchAccount, Domain, &cchDomain, &Use) &&
        wcslen(Domain) + 1 + wcslen(Account) < cchExpected)
    {
        wcscpy(Expected, Domain);
        wcscat(Expected, L"\\");
        wcscat(Expected, Account);
        Ok = TRUE;
    }

    CloseHandle(Token);
    return Ok;
}

START_TEST(GetUserNameEx)
{
    WCHAR Expected[256 + 1 + UNLEN + 1];
    WCHAR Name[256 + 1 + UNLEN + 1];
    ULONG Size;
    BOOLEAN Ret;

    if (!GetTokenUserSamName(Expected, ARRAYSIZE(Expected)))
    {
        skip("Could not resolve the token user name\n");
        return;
    }

    /* NameSamCompatible must equal domain\account from the token SID, so a
     * service account is reported as e.g. NT AUTHORITY\SYSTEM, not <machine>\. */
    Size = ARRAYSIZE(Name);
    SetLastError(0xdeadbeef);
    Ret = GetUserNameExW(NameSamCompatible, Name, &Size);
    ok(Ret, "GetUserNameExW failed: %lu\n", GetLastError());
    if (Ret)
    {
        ok(!_wcsicmp(Name, Expected), "got %s, expected %s\n",
           wine_dbgstr_w(Name), wine_dbgstr_w(Expected));
        ok(Size == (ULONG)wcslen(Name), "size %lu, len %lu\n", Size, (ULONG)wcslen(Name));
    }

    /* Size contract: a too-small buffer fails with ERROR_MORE_DATA and reports
     * the required length (including the terminating NUL). */
    Size = 1;
    SetLastError(0xdeadbeef);
    Ret = GetUserNameExW(NameSamCompatible, Name, &Size);
    ok(!Ret, "GetUserNameExW succeeded with a 1-char buffer\n");
    ok(GetLastError() == ERROR_MORE_DATA, "error %lu\n", GetLastError());
    ok(Size == (ULONG)wcslen(Expected) + 1, "required size %lu, expected %lu\n",
       Size, (ULONG)wcslen(Expected) + 1);
}
