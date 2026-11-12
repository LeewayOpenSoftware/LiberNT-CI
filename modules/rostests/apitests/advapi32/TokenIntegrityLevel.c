/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the integrity level of a token, of a child process and of the label names
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>
#include <sddl.h>

static DWORD
GetTokenIntegrityRid(HANDLE Token)
{
    BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    DWORD Length = 0;

    if (!GetTokenInformation(Token, TokenIntegrityLevel, Label, sizeof(Buffer), &Length))
    {
        ok(0, "GetTokenInformation(TokenIntegrityLevel) failed: %lu\n", GetLastError());
        return MAXDWORD;
    }
    return *GetSidSubAuthority(Label->Label.Sid, *GetSidSubAuthorityCount(Label->Label.Sid) - 1);
}

static DWORD
GetExpectedRid(HANDLE Token)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    PSID Admins = NULL, System = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    DWORD Length, Rid = SECURITY_MANDATORY_MEDIUM_RID;
    BOOL IsAdmin = FALSE;

    ok(GetTokenInformation(Token, TokenUser, User, sizeof(Buffer), &Length),
       "GetTokenInformation(TokenUser) failed: %lu\n", GetLastError());
    AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &System);
    AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                             0, 0, 0, 0, 0, 0, &Admins);
    if (System && EqualSid(User->User.Sid, System))
        Rid = SECURITY_MANDATORY_SYSTEM_RID;
    else if (Admins && CheckTokenMembership(NULL, Admins, &IsAdmin) && IsAdmin)
        Rid = SECURITY_MANDATORY_HIGH_RID;
    if (Admins)
        FreeSid(Admins);
    if (System)
        FreeSid(System);
    return Rid;
}

static VOID
TestOwnToken(VOID)
{
    HANDLE Token;
    DWORD Rid, Expected;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    Rid = GetTokenIntegrityRid(Token);
    Expected = GetExpectedRid(Token);
    ok(Rid == Expected, "Process token integrity 0x%lx, expected 0x%lx\n", Rid, Expected);
    CloseHandle(Token);
}

static VOID
TestChildProcess(PCSTR Self)
{
    CHAR CommandLine[MAX_PATH * 2];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Token;
    DWORD ExitCode = MAXDWORD, Rid;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    Rid = GetTokenIntegrityRid(Token);
    CloseHandle(Token);

    sprintf(CommandLine, "\"%s\" TokenIntegrityLevel child", Self);
    if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcessA failed: %lu\n", GetLastError());
        return;
    }
    ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "The child did not exit\n");
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
    ok(ExitCode == Rid, "Child integrity 0x%lx, parent 0x%lx\n", ExitCode, Rid);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

static VOID
TestLabelNames(VOID)
{
    static const struct
    {
        PCWSTR Sid;
        PCWSTR Name;
    } Labels[] =
    {
        { L"S-1-16-0", L"Untrusted Mandatory Level" },
        { L"S-1-16-4096", L"Low Mandatory Level" },
        { L"S-1-16-8192", L"Medium Mandatory Level" },
        { L"S-1-16-8448", L"Medium Plus Mandatory Level" },
        { L"S-1-16-12288", L"High Mandatory Level" },
        { L"S-1-16-16384", L"System Mandatory Level" },
        { L"S-1-16-20480", L"Protected Process Mandatory Level" },
    };
    WCHAR Name[256], Domain[256];
    DWORD NameLength, DomainLength;
    SID_NAME_USE Use;
    PSID Sid;
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Labels); i++)
    {
        if (!ConvertStringSidToSidW(Labels[i].Sid, &Sid))
        {
            ok(0, "ConvertStringSidToSidW(%S) failed: %lu\n", Labels[i].Sid, GetLastError());
            continue;
        }
        NameLength = ARRAYSIZE(Name);
        DomainLength = ARRAYSIZE(Domain);
        Name[0] = Domain[0] = UNICODE_NULL;
        Use = SidTypeUnknown;
        ok(LookupAccountSidW(NULL, Sid, Name, &NameLength, Domain, &DomainLength, &Use),
           "LookupAccountSidW(%S) failed: %lu\n", Labels[i].Sid, GetLastError());
        ok(!wcscmp(Domain, L"Mandatory Label"), "%S: domain '%S'\n", Labels[i].Sid, Domain);
        ok(!wcscmp(Name, Labels[i].Name), "%S: name '%S', expected '%S'\n", Labels[i].Sid, Name, Labels[i].Name);
        ok(Use == SidTypeLabel, "%S: use %d, expected SidTypeLabel\n", Labels[i].Sid, Use);
        LocalFree(Sid);
    }
}

START_TEST(TokenIntegrityLevel)
{
    HANDLE Token;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
            ExitProcess(MAXDWORD);
        ExitProcess(GetTokenIntegrityRid(Token));
    }

    TestOwnToken();
    TestChildProcess(argv[0]);
    TestLabelNames();
}
