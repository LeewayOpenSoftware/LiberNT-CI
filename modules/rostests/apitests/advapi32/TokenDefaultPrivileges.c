/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the default privileges of an interactive logon token
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static BOOL
IsMember(DWORD Rid)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID Sid;
    BOOL Member = FALSE;

    if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, Rid, 0, 0, 0, 0, 0, 0, &Sid))
        return FALSE;
    if (!CheckTokenMembership(NULL, Sid, &Member))
        Member = FALSE;
    FreeSid(Sid);
    return Member;
}

static PLUID_AND_ATTRIBUTES
FindPrivilege(PTOKEN_PRIVILEGES Privileges, PCWSTR Name)
{
    LUID Luid;
    DWORD i;

    if (!LookupPrivilegeValueW(NULL, Name, &Luid))
    {
        ok(0, "LookupPrivilegeValueW(%S) failed: %lu\n", Name, GetLastError());
        return NULL;
    }
    for (i = 0; i < Privileges->PrivilegeCount; i++)
    {
        if (Privileges->Privileges[i].Luid.LowPart == Luid.LowPart &&
            Privileges->Privileges[i].Luid.HighPart == Luid.HighPart)
            return &Privileges->Privileges[i];
    }
    return NULL;
}

static VOID
ExpectPrivilege(PTOKEN_PRIVILEGES Privileges, PCWSTR Name)
{
    PLUID_AND_ATTRIBUTES Entry = FindPrivilege(Privileges, Name);

    ok(Entry != NULL, "%S is missing from the token\n", Name);
    if (Entry)
        ok(!(Entry->Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT), "%S is enabled by default\n", Name);
}

START_TEST(TokenDefaultPrivileges)
{
    BYTE Buffer[4096];
    PTOKEN_PRIVILEGES Privileges = (PTOKEN_PRIVILEGES)Buffer;
    PLUID_AND_ATTRIBUTES Entry;
    HANDLE Token;
    DWORD Length;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    if (!GetTokenInformation(Token, TokenPrivileges, Privileges, sizeof(Buffer), &Length))
    {
        ok(0, "GetTokenInformation(TokenPrivileges) failed: %lu\n", GetLastError());
        CloseHandle(Token);
        return;
    }
    CloseHandle(Token);

    if (IsMember(DOMAIN_ALIAS_RID_USERS) || IsMember(DOMAIN_ALIAS_RID_ADMINS))
    {
        ExpectPrivilege(Privileges, L"SeIncreaseWorkingSetPrivilege");
        ExpectPrivilege(Privileges, L"SeTimeZonePrivilege");
    }
    if (IsMember(DOMAIN_ALIAS_RID_ADMINS))
    {
        ExpectPrivilege(Privileges, L"SeDelegateSessionUserImpersonatePrivilege");
        Entry = FindPrivilege(Privileges, L"SeCreatePagefilePrivilege");
        ok(Entry != NULL, "SeCreatePagefilePrivilege is missing from an administrator token\n");
        if (Entry)
            ok(!(Entry->Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT),
               "SeCreatePagefilePrivilege is enabled by default\n");
    }
    else
    {
        skip("Not an administrator: SeDelegateSessionUserImpersonatePrivilege not checked\n");
    }
}
