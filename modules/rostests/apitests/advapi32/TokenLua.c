/*
 * PROJECT:     LiberNT API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the LUA token filtering and the UAC linked pair
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <lm.h>
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
RunChild(DWORD ParentPid, HANDLE InheritedImpersonation, BOOL ExpectLink)
{
    HANDLE Parent, Token = NULL, ThreadToken = NULL;
    SECURITY_IMPERSONATION_LEVEL Level = SecurityAnonymous;
    DWORD Size, Value, Failures = 0;

    /* Our own pair, seen without SeTcbPrivilege */
    if (ExpectLink && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        TOKEN_LINKED_TOKEN Link = { NULL };
        TOKEN_TYPE Type = TokenPrimary;
        HANDLE Copy = NULL;

        if (!GetTokenDword(Token, TokenElevationType, &Value) || Value != TokenElevationTypeLimited)
            Failures |= 0x2000;
        if (!GetTokenInformation(Token, TokenLinkedToken, &Link, sizeof(Link), &Size) || !Link.LinkedToken)
            Failures |= 0x80;
        else
        {
            if (!GetTokenInformation(Link.LinkedToken, TokenType, &Type, sizeof(Type), &Size) || Type != TokenImpersonation)
                Failures |= 0x200;
            if (!GetTokenInformation(Link.LinkedToken, TokenImpersonationLevel, &Level, sizeof(Level), &Size) ||
                Level != SecurityIdentification)
                Failures |= 0x400;
            if (DuplicateTokenEx(Link.LinkedToken, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &Copy))
            {
                Failures |= 0x800;
                CloseHandle(Copy);
            }
            if (SetThreadToken(NULL, Link.LinkedToken))
            {
                if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &ThreadToken))
                {
                    if (!GetTokenInformation(ThreadToken, TokenImpersonationLevel, &Level, sizeof(Level), &Size) ||
                        Level > SecurityIdentification)
                        Failures |= 0x1000;
                    CloseHandle(ThreadToken);
                    ThreadToken = NULL;
                }
                RevertToSelf();
            }
            CloseHandle(Link.LinkedToken);
        }
        CloseHandle(Token);
        Token = NULL;
    }

    if (ParentPid == 0)
        return Failures;

    Parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ParentPid);
    if (!Parent)
        return Failures | 0x100;

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
    if (InheritedImpersonation == NULL)
        return Failures;
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

    StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" TokenLua child %lu %p 0", Exe, GetCurrentProcessId(), Impersonation);
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

#define PAIR_TEST_USER     L"LntLuaPairTest"
#define PAIR_TEST_PASSWORD L"Lnt!LuaPair#2026q"

static
LUID
GetLogonId(HANDLE Token)
{
    BYTE Buffer[sizeof(TOKEN_STATISTICS)];
    LUID Zero = {0, 0};
    DWORD Size;

    if (!GetTokenInformation(Token, TokenStatistics, Buffer, sizeof(Buffer), &Size))
        return Zero;
    return ((PTOKEN_STATISTICS)Buffer)->AuthenticationId;
}

/* Two logon sessions of one user: the LUA token of the second is paired with the
 * full token of the first, as LSA does it, then the pair is broken with a NULL link.
 * Needs SeTcbPrivilege and the right to create a local user. */
static
void
TestTwoSessions(void)
{
    USER_INFO_1 User = {0};
    HANDLE T1 = NULL, T2 = NULL, Lua2 = NULL, Linked = NULL;
    TOKEN_LINKED_TOKEN Link;
    LUID SessionA, SessionB, Id;
    DWORD Value, Size, Error;
    NET_API_STATUS NetStatus;
    BOOL Ret;

    NetUserDel(NULL, PAIR_TEST_USER);
    User.usri1_name = PAIR_TEST_USER;
    User.usri1_password = PAIR_TEST_PASSWORD;
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    NetStatus = NetUserAdd(NULL, 1, (LPBYTE)&User, NULL);
    if (NetStatus != NERR_Success)
    {
        skip("NetUserAdd failed %lu; the two-session checks need a local user\n", NetStatus);
        return;
    }

    Ret = LogonUserW(PAIR_TEST_USER, L".", PAIR_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T1) &&
          LogonUserW(PAIR_TEST_USER, L".", PAIR_TEST_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &T2);
    ok(Ret, "LogonUserW failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    SessionA = GetLogonId(T1);
    SessionB = GetLogonId(T2);
    ok(SessionA.LowPart != SessionB.LowPart || SessionA.HighPart != SessionB.HighPart,
       "Two logons share logon id %08lx:%08lx\n", SessionA.HighPart, SessionA.LowPart);

    Ret = CreateRestrictedToken(T2, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &Lua2);
    ok(Ret, "CreateRestrictedToken(T2, LUA_TOKEN) failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    /* Link across the two sessions */
    Link.LinkedToken = T1;
    Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
    ok(Ret, "Linking across two sessions failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    ok(GetTokenDword(Lua2, TokenElevationType, &Value) && Value == TokenElevationTypeLimited,
       "Limited side after the two-session link: %lu\n", Value);
    ok(GetTokenDword(T1, TokenElevationType, &Value) && Value == TokenElevationTypeFull,
       "Full side after the two-session link: %lu\n", Value);

    Ret = GetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link), &Size);
    ok(Ret, "Limited -> full across sessions failed: %lu\n", GetLastError());
    if (Ret)
    {
        Linked = Link.LinkedToken;
        Id = GetLogonId(Linked);
        ok(Id.LowPart == SessionA.LowPart && Id.HighPart == SessionA.HighPart,
           "The elevated side is in session %08lx:%08lx, expected %08lx:%08lx\n",
           Id.HighPart, Id.LowPart, SessionA.HighPart, SessionA.LowPart);
        /* The pair's full side belongs to a standard user here: linked, not elevated */
        ok(GetTokenDword(Linked, TokenElevation, &Value) && Value == 0,
           "A standard user's full side reports elevation %lu\n", Value);
        CloseHandle(Linked);
        Linked = NULL;
    }

    Ret = GetTokenInformation(T1, TokenLinkedToken, &Link, sizeof(Link), &Size);
    ok(Ret, "Full -> limited across sessions failed: %lu\n", GetLastError());
    if (Ret)
    {
        Linked = Link.LinkedToken;
        Id = GetLogonId(Linked);
        ok(Id.LowPart == SessionB.LowPart && Id.HighPart == SessionB.HighPart,
           "The limited side is in session %08lx:%08lx, expected %08lx:%08lx\n",
           Id.HighPart, Id.LowPart, SessionB.HighPart, SessionB.LowPart);
        ok(GetTokenDword(Linked, TokenElevationType, &Value) && Value == TokenElevationTypeLimited,
           "The limited side reports %lu\n", Value);
        CloseHandle(Linked);
        Linked = NULL;
    }

    /* A child of the limited token sees its own pair without SeTcbPrivilege */
    {
        STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
        PROCESS_INFORMATION ProcessInfo;
        WCHAR Exe[MAX_PATH], CommandLine[MAX_PATH + 64];
        DWORD ExitCode = 0xffffffff;

        if (GetModuleFileNameW(NULL, Exe, _countof(Exe)))
        {
            StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" TokenLua child 0 0 1", Exe);
            Ret = CreateProcessAsUserW(Lua2, Exe, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL,
                                       &StartupInfo, &ProcessInfo);
            ok(Ret, "CreateProcessAsUserW(two-session LUA token) failed: %lu\n", GetLastError());
            if (Ret)
            {
                ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "The linked child did not finish\n");
                ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
                ok(ExitCode == 0, "Linked child failures: 0x%lx (80 no link, 200 not impersonation, 400 level, "
                   "800 primary copy granted, 1000 thread level above identification, 2000 type not limited)\n", ExitCode);
                CloseHandle(ProcessInfo.hThread);
                CloseHandle(ProcessInfo.hProcess);
            }
        }
    }

    /* A NULL link breaks the pair in both sessions */
    Link.LinkedToken = NULL;
    Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
    ok(Ret, "Unlinking with a NULL handle failed: %lu\n", GetLastError());
    Ret = GetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link), &Size);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_NO_SUCH_LOGON_SESSION, "Limited side still linked: ret %d, error %lu\n", Ret, Error);
    Ret = GetTokenInformation(T1, TokenLinkedToken, &Link, sizeof(Link), &Size);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_NO_SUCH_LOGON_SESSION, "Full side still linked: ret %d, error %lu\n", Ret, Error);
    ok(GetTokenDword(T1, TokenElevationType, &Value) && Value == TokenElevationTypeDefault,
       "Full side after the unlink: %lu\n", Value);
    ok(GetTokenDword(Lua2, TokenElevationType, &Value) && Value == TokenElevationTypeLimited,
       "LUA token after the unlink: %lu\n", Value);

    /* And the sessions take a new pair afterwards */
    Link.LinkedToken = T1;
    Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
    ok(Ret, "Relinking after the unlink failed: %lu\n", GetLastError());

    /* The unlink may drop the last reference on the elevated token */
    CloseHandle(T1);
    T1 = NULL;
    Link.LinkedToken = NULL;
    Ret = SetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link));
    ok(Ret, "Unlinking after the elevated handle is closed failed: %lu\n", GetLastError());
    Ret = GetTokenInformation(Lua2, TokenLinkedToken, &Link, sizeof(Link), &Size);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_NO_SUCH_LOGON_SESSION, "Still linked after the last unlink: ret %d, error %lu\n", Ret, Error);

Cleanup:
    if (Lua2)
        CloseHandle(Lua2);
    if (T2)
        CloseHandle(T2);
    if (T1)
        CloseHandle(T1);
    NetUserDel(NULL, PAIR_TEST_USER);
}

/* CheckTokenMembership takes an impersonation token; duplicate a primary one for it */
static
BOOL
IsMemberViaImpersonation(HANDLE Token, PSID Sid, PBOOL IsMember)
{
    HANDLE Imp;
    BOOL Ret;

    *IsMember = FALSE;
    if (!DuplicateTokenEx(Token, TOKEN_QUERY, NULL, SecurityIdentification, TokenImpersonation, &Imp))
        return FALSE;
    Ret = CheckTokenMembership(Imp, Sid, IsMember);
    CloseHandle(Imp);
    return Ret;
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
    PTOKEN_STATISTICS Statistics = (PTOKEN_STATISTICS)Buffer;
    TOKEN_LINKED_TOKEN Linked;
    TOKEN_PRIVILEGES Enable;
    DWORD Size, Value, i, Error;
    BOOL Ret, IsAdmin = FALSE, Found = FALSE;

    if (Argc >= 6 && !strcmp(Argv[2], "child"))
    {
        ExitProcess(RunChild(strtoul(Argv[3], NULL, 10),
                             (HANDLE)(ULONG_PTR)strtoull(Argv[4], NULL, 16),
                             Argv[5][0] == '1'));
    }

    ok(AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                0, 0, 0, 0, 0, 0, &AdminsSid), "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    ok(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Process),
       "OpenProcessToken failed: %lu\n", GetLastError());
    if (!AdminsSid || !Process)
        goto Cleanup;

    /* CheckTokenMembership wants an impersonation token, as on Windows */
    ok(IsMemberViaImpersonation(Process, AdminsSid, &IsAdmin), "CheckTokenMembership failed: %lu\n", GetLastError());
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
    /* An already limited runner carries the group deny-only; the LUA copy keeps it that way */
    ok(!IsAdmin || Found, "Administrators enabled in the process token but missing from the LUA token\n");

    Value = 0xdead;
    ok(IsMemberViaImpersonation(Lua, AdminsSid, (PBOOL)&Value), "CheckTokenMembership(LUA) failed: %lu\n", GetLastError());
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

    /* No pair yet, unless the runner's own logon already is one (a limited console session) */
    Linked.LinkedToken = (HANDLE)0xdeadbeef;
    Ret = GetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked), &Size);
    Error = GetLastError();
    if (GetTokenDword(Process, TokenElevationType, &Value) && Value == TokenElevationTypeLimited)
    {
        ok(Ret, "Query in a split logon: ret %d, error %lu\n", Ret, Error);
        if (Ret && Linked.LinkedToken != (HANDLE)0xdeadbeef)
            CloseHandle(Linked.LinkedToken);
    }
    else
    {
        ok(!Ret && Error == ERROR_NO_SUCH_LOGON_SESSION, "Unpaired query: ret %d, error %lu\n", Ret, Error);
        ok(Linked.LinkedToken == (HANDLE)0xdeadbeef, "Unpaired query wrote %p\n", Linked.LinkedToken);
    }

    /* Linking needs SeTcbPrivilege */
    Enable.PrivilegeCount = 1;
    Enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, L"SeTcbPrivilege", &Enable.Privileges[0].Luid);
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

    /* The System session never carries a pair; the pairing checks need a logon session */
    Ret = GetTokenInformation(Process, TokenStatistics, Statistics, sizeof(Buffer), &Size);
    ok(Ret, "TokenStatistics failed: %lu\n", GetLastError());
    if (Ret && Statistics->AuthenticationId.HighPart == 0 && Statistics->AuthenticationId.LowPart <= 999)
    {
        Linked.LinkedToken = Process;
        Ret = SetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked));
        Error = GetLastError();
        ok(!Ret && Error == ERROR_INVALID_PARAMETER, "Pair on a well-known session: ret %d, error %lu\n", Ret, Error);
        skip("The runner is in a well-known logon session; the same-session pairing checks need a LogonUser session\n");
        TestTwoSessions();
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

    /* Without SeTcbPrivilege (impersonating the LUA token) the query only looks */
    Ret = ImpersonateLoggedOnUser(Lua);
    ok(Ret, "ImpersonateLoggedOnUser(LUA) failed: %lu\n", GetLastError());
    if (Ret)
    {
        HANDLE Weak = NULL, Copy = NULL;
        TOKEN_TYPE Type = TokenPrimary;
        SECURITY_IMPERSONATION_LEVEL Level = SecurityAnonymous;

        Linked.LinkedToken = NULL;
        Ret = GetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked), &Size);
        ok(Ret && Linked.LinkedToken != NULL, "Query without SeTcb: ret %d, error %lu\n", Ret, GetLastError());
        Weak = Ret ? Linked.LinkedToken : NULL;
        if (Weak)
        {
            ok(GetTokenInformation(Weak, TokenType, &Type, sizeof(Type), &Size) && Type == TokenImpersonation,
               "Without SeTcb the linked token is not an impersonation token: %d\n", Type);
            ok(GetTokenInformation(Weak, TokenImpersonationLevel, &Level, sizeof(Level), &Size) &&
               Level == SecurityIdentification, "Without SeTcb the level is %d, expected identification\n", Level);
            ok(!DuplicateTokenEx(Weak, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &Copy),
               "A primary copy of the identification token was granted\n");
            if (Copy) CloseHandle(Copy);
            Copy = NULL;
            ok(!DuplicateToken(Weak, SecurityIdentification, &Copy),
               "The identification token handle allows TOKEN_DUPLICATE\n");
            if (Copy) CloseHandle(Copy);
            CloseHandle(Weak);
        }
        RevertToSelf();
    }

    /* A second pair on the same session is refused */
    Linked.LinkedToken = Process;
    Ret = SetTokenInformation(Lua, TokenLinkedToken, &Linked, sizeof(Linked));
    ok(!Ret, "A second link on the same session succeeded\n");

    TestTwoSessions();

Cleanup:
    if (Lua)
        CloseHandle(Lua);
    if (Process)
        CloseHandle(Process);
    if (AdminsSid)
        FreeSid(AdminsSid);
}

