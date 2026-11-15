/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     CreateProcessWithLogonW starts the process as the named user, or as the caller with LOGON_NETCREDENTIALS_ONLY
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <lm.h>
#include <sddl.h>

#define TEST_USER     L"LntWithLogonTest"
#define TEST_PASSWORD L"Lnt!WithLogon#2026q"

/* What the child reports about its own token (bit mask in the exit code) */
#define CHILD_OTHER_USER      0x1
#define CHILD_NO_LEVEL        0x2
#define CHILD_NO_LABEL_GROUP  0x4
#define CHILD_BROKEN          0x8

static
DWORD
RunChild(
    _In_ PCSTR ExpectedSid)
{
    union
    {
        TOKEN_USER User;
        BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } User;
    union
    {
        TOKEN_MANDATORY_LABEL Label;
        BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    } Label;
    BYTE GroupBuffer[4096];
    PTOKEN_GROUPS Groups = (PTOKEN_GROUPS)GroupBuffer;
    HANDLE hToken;
    LPSTR Sid = NULL;
    DWORD Length, i, Result = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return CHILD_BROKEN;
    if (!GetTokenInformation(hToken, TokenUser, &User, sizeof(User), &Length) ||
        !ConvertSidToStringSidA(User.User.User.Sid, &Sid))
    {
        CloseHandle(hToken);
        return CHILD_BROKEN;
    }
    if (strcmp(Sid, ExpectedSid) != 0)
        Result |= CHILD_OTHER_USER;
    LocalFree(Sid);

    if (!GetTokenInformation(hToken, TokenIntegrityLevel, &Label, sizeof(Label), &Length))
        Result |= CHILD_NO_LEVEL;

    Result |= CHILD_NO_LABEL_GROUP;
    if (GetTokenInformation(hToken, TokenGroups, Groups, sizeof(GroupBuffer), &Length))
    {
        for (i = 0; i < Groups->GroupCount; i++)
        {
            if (Groups->Groups[i].Attributes & SE_GROUP_INTEGRITY)
                Result &= ~CHILD_NO_LABEL_GROUP;
        }
    }
    CloseHandle(hToken);
    return Result;
}

static
BOOL
StartAs(
    _In_ PCWSTR User,
    _In_ PCWSTR Password,
    _In_ DWORD LogonFlags,
    _In_ PCSTR ExpectedSid,
    _Out_ PDWORD ExitCode)
{
    STARTUPINFOW StartupInfo = {sizeof(StartupInfo)};
    PROCESS_INFORMATION ProcessInfo = {0};
    WCHAR CommandLine[MAX_PATH + 128], Exe[MAX_PATH];
    BOOL Result;

    *ExitCode = MAXDWORD;
    GetModuleFileNameW(NULL, Exe, ARRAYSIZE(Exe));
    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%ls\" CreateProcessWithLogon child %hs", Exe, ExpectedSid);
    Result = CreateProcessWithLogonW(User, L".", Password, LogonFlags, NULL, CommandLine, CREATE_NO_WINDOW,
                                     NULL, NULL, &StartupInfo, &ProcessInfo);
    if (!Result)
        return FALSE;
    if (WaitForSingleObject(ProcessInfo.hProcess, 60000) == WAIT_OBJECT_0)
        GetExitCodeProcess(ProcessInfo.hProcess, ExitCode);
    else
        TerminateProcess(ProcessInfo.hProcess, 0);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    return TRUE;
}

START_TEST(CreateProcessWithLogon)
{
    USER_INFO_1 UserInfo = {0};
    union
    {
        TOKEN_USER User;
        BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } Caller;
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    WCHAR Domain[256];
    SID_NAME_USE Use;
    DWORD SidSize = sizeof(SidBuffer), DomainSize = ARRAYSIZE(Domain), Length, ExitCode;
    LPSTR UserSid = NULL, CallerSid = NULL;
    HANDLE hToken;
    NET_API_STATUS Status;
    char **argv;
    BOOL Result;

    if (winetest_get_mainargs(&argv) >= 4 && !strcmp(argv[2], "child"))
        ExitProcess(RunChild(argv[3]));

    UserInfo.usri1_name = TEST_USER;
    UserInfo.usri1_password = TEST_PASSWORD;
    UserInfo.usri1_priv = USER_PRIV_USER;
    UserInfo.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    NetUserDel(NULL, TEST_USER);
    Status = NetUserAdd(NULL, 1, (LPBYTE)&UserInfo, NULL);
    if (Status != NERR_Success)
    {
        skip("NetUserAdd failed %lu; this test needs to create a local user\n", Status);
        return;
    }

    Result = LookupAccountNameW(NULL, TEST_USER, SidBuffer, &SidSize, Domain, &DomainSize, &Use) &&
             ConvertSidToStringSidA(SidBuffer, &UserSid) &&
             OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken);
    ok(Result, "Resolving the test user and the caller failed %lu\n", GetLastError());
    if (!Result)
        goto Cleanup;
    Result = GetTokenInformation(hToken, TokenUser, &Caller, sizeof(Caller), &Length) &&
             ConvertSidToStringSidA(Caller.User.User.Sid, &CallerSid);
    CloseHandle(hToken);
    ok(Result, "Reading the caller's user failed %lu\n", GetLastError());
    if (!Result)
        goto Cleanup;

    /* The named user, with the credentials checked */
    Result = StartAs(TEST_USER, TEST_PASSWORD, 0, UserSid, &ExitCode);
    ok(Result, "CreateProcessWithLogonW(%ls) failed %lu\n", TEST_USER, GetLastError());
    if (Result)
    {
        ok(!(ExitCode & CHILD_OTHER_USER), "the child does not run as %ls (child code 0x%lx)\n", TEST_USER, ExitCode);
        ok(!(ExitCode & (CHILD_NO_LEVEL | CHILD_NO_LABEL_GROUP)), "the child's token has no integrity label (child code 0x%lx)\n", ExitCode);
        ok(!(ExitCode & CHILD_BROKEN) && ExitCode != MAXDWORD, "the child could not look at its token (0x%lx)\n", ExitCode);
    }

    Result = StartAs(TEST_USER, L"not the password", 0, UserSid, &ExitCode);
    ok(!Result && GetLastError() == ERROR_LOGON_FAILURE, "wrong password: %d, error %lu\n", Result, GetLastError());

    /* Network-only credentials: the process is the caller's, the credentials are for the network */
    Result = StartAs(TEST_USER, TEST_PASSWORD, LOGON_NETCREDENTIALS_ONLY, CallerSid, &ExitCode);
    ok(Result, "CreateProcessWithLogonW(LOGON_NETCREDENTIALS_ONLY) failed %lu\n", GetLastError());
    if (Result)
    {
        ok(!(ExitCode & CHILD_OTHER_USER), "with LOGON_NETCREDENTIALS_ONLY the child is not the caller (child code 0x%lx)\n", ExitCode);
        ok(!(ExitCode & (CHILD_NO_LEVEL | CHILD_NO_LABEL_GROUP)), "the caller's copy lost its integrity label (child code 0x%lx)\n", ExitCode);
    }

Cleanup:
    if (UserSid) LocalFree(UserSid);
    if (CallerSid) LocalFree(CallerSid);
    NetUserDel(NULL, TEST_USER);
}
