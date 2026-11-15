/*
 * PROJECT:     LiberNT API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the LUA token filtering and the UAC linked pair
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <ndk/setypes.h>

#ifndef TokenElevationTypeLimited
#define TokenElevationTypeFull    2
#define TokenElevationTypeLimited 3
#endif

static
BOOL
GetTokenDword(HANDLE Token, TOKEN_INFORMATION_CLASS Class, DWORD *Value)
{
    DWORD Size = 0;

    *Value = 0;
    return GetTokenInformation(Token, Class, Value, sizeof(*Value), &Size);
}

static
BOOL
IsLuaPrivilege(const LUID *Luid)
{
    if (Luid->HighPart != 0)
        return FALSE;
    return Luid->LowPart == SE_CHANGE_NOTIFY_PRIVILEGE ||
           Luid->LowPart == SE_SHUTDOWN_PRIVILEGE ||
           Luid->LowPart == SE_UNDOCK_PRIVILEGE ||
           Luid->LowPart == SE_INC_WORKING_SET_PRIVILEGE ||
           Luid->LowPart == SE_TIME_ZONE_PRIVILEGE;
}

START_TEST(TokenLua)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    HANDLE Process = NULL, Lua = NULL;
    PSID AdminsSid = NULL;
    BYTE Buffer[4096];
    PTOKEN_GROUPS Groups = (PTOKEN_GROUPS)Buffer;
    PTOKEN_PRIVILEGES Privileges = (PTOKEN_PRIVILEGES)Buffer;
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    TOKEN_LINKED_TOKEN Linked;
    TOKEN_PRIVILEGES Enable;
    DWORD Size, Value, i, Error;
    BOOL Ret, IsAdmin = FALSE, Found = FALSE;

    ok(AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                0, 0, 0, 0, 0, 0, &AdminsSid), "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_PRIVILEGES,
                        &Process), "OpenProcessToken failed: %lu\n", GetLastError());
    if (!AdminsSid || !Process)
        goto Cleanup;

    ok(CheckTokenMembership(Process, AdminsSid, &IsAdmin), "CheckTokenMembership failed: %lu\n", GetLastError());
    trace("The process token %s a member of Administrators\n", IsAdmin ? "is" : "is not");

    /* The LUA transform itself */
    Ret = CreateRestrictedToken(Process, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &Lua);
    ok(Ret, "CreateRestrictedToken(LUA_TOKEN) failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    Ret = GetTokenInformation(Lua, TokenGroups, Groups, sizeof(Buffer), &Size);
    ok(Ret, "TokenGroups failed: %lu\n", GetLastError());
    for (i = 0; Ret && i < Groups->GroupCount; i++)
    {
        if (!EqualSid(Groups->Groups[i].Sid, AdminsSid))
            continue;
        Found = TRUE;
        ok(Groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY,
           "Administrators is not deny-only: 0x%lx\n", Groups->Groups[i].Attributes);
        ok(!(Groups->Groups[i].Attributes & SE_GROUP_ENABLED),
           "Administrators is still enabled: 0x%lx\n", Groups->Groups[i].Attributes);
    }
    ok(Found == IsAdmin, "Administrators present in the LUA token: %d, in the process token: %d\n", Found, IsAdmin);

    Value = 0xdead;
    ok(CheckTokenMembership(Lua, AdminsSid, (PBOOL)&Value), "CheckTokenMembership(LUA) failed: %lu\n", GetLastError());
    ok(Value == FALSE, "The LUA token still passes an Administrators membership check\n");

    Ret = GetTokenInformation(Lua, TokenPrivileges, Privileges, sizeof(Buffer), &Size);
    ok(Ret, "TokenPrivileges failed: %lu\n", GetLastError());
    for (i = 0; Ret && i < Privileges->PrivilegeCount; i++)
    {
        ok(IsLuaPrivilege(&Privileges->Privileges[i].Luid), "Privilege %lu survived the LUA filtering\n",
           Privileges->Privileges[i].Luid.LowPart);
    }

    Ret = GetTokenInformation(Lua, TokenIntegrityLevel, Label, sizeof(Buffer), &Size);
    ok(Ret, "TokenIntegrityLevel failed: %lu\n", GetLastError());
    if (Ret)
    {
        DWORD Rid = *GetSidSubAuthority(Label->Label.Sid, *GetSidSubAuthorityCount(Label->Label.Sid) - 1);
        ok(Rid <= SECURITY_MANDATORY_MEDIUM_RID, "The LUA token is above Medium: 0x%lx\n", Rid);
    }

    ok(GetTokenDword(Lua, TokenHasRestrictions, &Value) && Value == 1, "TokenHasRestrictions = %lu\n", Value);
    ok(GetTokenDword(Lua, TokenElevation, &Value) && Value == 0, "TokenElevation = %lu\n", Value);
    ok(GetTokenDword(Lua, TokenElevationType, &Value) && Value == TokenElevationTypeLimited,
       "TokenElevationType = %lu, expected %u\n", Value, TokenElevationTypeLimited);

    /* No pair yet */
    Linked.LinkedToken = (HANDLE)0xdeadbeef;
    Ret = GetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked), &Size);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_NO_SUCH_LOGON_SESSION, "Unpaired query: ret %d, error %lu\n", Ret, Error);
    ok(Linked.LinkedToken == (HANDLE)0xdeadbeef, "Unpaired query wrote %p\n", Linked.LinkedToken);

    /* Linking needs SeTcbPrivilege */
    Enable.PrivilegeCount = 1;
    Enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, SE_TCB_NAME, &Enable.Privileges[0].Luid);
    AdjustTokenPrivileges(Process, FALSE, &Enable, 0, NULL, NULL);
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        Linked.LinkedToken = Process;
        Ret = SetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked));
        Error = GetLastError();
        ok(!Ret && Error == ERROR_PRIVILEGE_NOT_HELD, "Link without SeTcb: ret %d, error %lu\n", Ret, Error);
        skip("SeTcbPrivilege is not held; the pairing checks need to run as SYSTEM\n");
        goto Cleanup;
    }

    Linked.LinkedToken = Process;
    Ret = SetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked));
    ok(Ret, "Linking the pair failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    ok(GetTokenDword(Lua, TokenElevationType, &Value) && Value == TokenElevationTypeLimited,
       "Limited side: TokenElevationType = %lu\n", Value);
    ok(GetTokenDword(Process, TokenElevationType, &Value) && Value == TokenElevationTypeFull,
       "Full side: TokenElevationType = %lu\n", Value);

    Linked.LinkedToken = NULL;
    Ret = GetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked), &Size);
    ok(Ret && Linked.LinkedToken != NULL, "Limited -> full query: ret %d, error %lu\n", Ret, GetLastError());
    if (Ret && Linked.LinkedToken)
    {
        ok(GetTokenDword(Linked.LinkedToken, TokenElevation, &Value) && Value == 1,
           "The linked token is not elevated: %lu\n", Value);
        CloseHandle(Linked.LinkedToken);
    }

    /* A second pair on the same session is refused */
    Linked.LinkedToken = Process;
    Ret = SetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked));
    ok(!Ret, "A second link on the same session succeeded\n");

Cleanup:
    if (Lua)
        CloseHandle(Lua);
    if (Process)
        CloseHandle(Process);
    if (AdminsSid)
        FreeSid(AdminsSid);
}

