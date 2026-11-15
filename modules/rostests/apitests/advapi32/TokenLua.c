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

/* Child side: runs with the LUA token. Bit mask of failures in the exit code. */
static
DWORD
RunChild(DWORD ParentPid, HANDLE InheritedImpersonation)
{
    HANDLE Parent, Token = NULL, ThreadToken = NULL;
    SECURITY_IMPERSONATION_LEVEL Level = SecurityAnonymous;
    DWORD Size, Failures = 0;

    Parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ParentPid);
    if (!Parent)
        return 0x100;

    /* Looking at the elevated token is fine, taking it is not */
    if (!OpenProcessToken(Parent, TOKEN_QUERY, &Token))
        Failures |= 0x1;
    else
        CloseHandle(Token);
    if (OpenProcessToken(Parent, TOKEN_DUPLICATE, &Token))
    {
        Failures |= 0x2;
        CloseHandle(Token);
    }
    else if (GetLastError() != ERROR_ACCESS_DENIED)
        Failures |= 0x4;
    if (OpenProcessToken(Parent, TOKEN_IMPERSONATE, &Token))
    {
        Failures |= 0x8;
        CloseHandle(Token);
    }
    CloseHandle(Parent);

    /* An elevated impersonation token handed to us only gives identification */
    if (!SetThreadToken(NULL, InheritedImpersonation))
        return Failures | 0x10;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &ThreadToken))
        Failures |= 0x20;
    else
    {
        if (!GetTokenInformation(ThreadToken, TokenImpersonationLevel, &Level, sizeof(Level), &Size) ||
            Level != SecurityIdentification)
        {
            Failures |= 0x40;
        }
        CloseHandle(ThreadToken);
    }
    RevertToSelf();
    return Failures;
}

static
void
TestMediumChild(HANDLE Process, HANDLE Lua)
{
    SECURITY_ATTRIBUTES Inheritable = { sizeof(Inheritable), NULL, TRUE };
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR Exe[MAX_PATH], CommandLine[MAX_PATH + 64];
    HANDLE Impersonation = NULL;
    DWORD ExitCode = 0xffffffff;
    BOOL Ret;

    if (!GetModuleFileNameW(NULL, Exe, _countof(Exe)))
    {
        skip("GetModuleFileNameW failed: %lu\n", GetLastError());
        return;
    }

    Ret = DuplicateTokenEx(Process, TOKEN_ALL_ACCESS, &Inheritable, SecurityImpersonation,
                           TokenImpersonation, &Impersonation);
    ok(Ret, "DuplicateTokenEx failed: %lu\n", GetLastError());
    if (!Ret)
        return;

    swprintf(CommandLine, L"\"%ls\" TokenLua child %lu %p", Exe, GetCurrentProcessId(), Impersonation);
    Ret = CreateProcessAsUserW(Lua, Exe, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL,
                               &StartupInfo, &ProcessInfo);
    ok(Ret, "CreateProcessAsUserW(LUA token) failed: %lu\n", GetLastError());
    if (Ret)
    {
        ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "The child did not finish\n");
        ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
        ok(ExitCode == 0, "Medium child failures: 0x%lx (1 query denied, 2 duplicate granted, 4 wrong error, "
           "8 impersonate granted, 10 SetThreadToken, 20 OpenThreadToken, 40 level not identification, 100 OpenProcess)\n",
           ExitCode);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
    CloseHandle(Impersonation);
}

START_TEST(TokenLua)
{
    char **Argv;
    int Argc = winetest_get_mainargs(&Argv);

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

    if (Argc >= 5 && !strcmp(Argv[2], "child"))
    {
        ExitProcess(RunChild(strtoul(Argv[3], NULL, 10), (HANDLE)(ULONG_PTR)strtoull(Argv[4], NULL, 16)));
    }

    ok(AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                0, 0, 0, 0, 0, 0, &AdminsSid), "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Process),
       "OpenProcessToken failed: %lu\n", GetLastError());
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

    /* What a Medium process of the same user can do with our elevated token */
    Ret = GetTokenInformation(Process, TokenIntegrityLevel, Label, sizeof(Buffer), &Size);
    if (Ret && *GetSidSubAuthority(Label->Label.Sid, *GetSidSubAuthorityCount(Label->Label.Sid) - 1) >= SECURITY_MANDATORY_HIGH_RID)
        TestMediumChild(Process, Lua);
    else
        skip("The test process is not High: the Medium child checks need an elevated runner\n");

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

