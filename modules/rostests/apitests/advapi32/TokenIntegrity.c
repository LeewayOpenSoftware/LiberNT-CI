/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the integrity level stamped on logon tokens
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>
#include <tlhelp32.h>

static DWORD GetTokenIntegrityRid(HANDLE hToken)
{
    BYTE Buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    DWORD cb = 0;

    if (!GetTokenInformation(hToken, TokenIntegrityLevel, Label, sizeof(Buffer), &cb))
    {
        ok(0, "TokenIntegrityLevel failed %lu\n", GetLastError());
        return MAXDWORD;
    }
    ok(Label->Label.Attributes & SE_GROUP_INTEGRITY, "attributes 0x%lx\n", Label->Label.Attributes);
    ok(*GetSidSubAuthorityCount(Label->Label.Sid) == 1, "subauthority count %u\n",
       *GetSidSubAuthorityCount(Label->Label.Sid));
    return *GetSidSubAuthority(Label->Label.Sid, 0);
}

static DWORD FindProcess(PCWSTR Name)
{
    PROCESSENTRY32W Entry = { sizeof(Entry) };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD Pid = 0;

    if (hSnap == INVALID_HANDLE_VALUE)
        return 0;
    if (Process32FirstW(hSnap, &Entry))
    {
        do
        {
            if (!_wcsicmp(Entry.szExeFile, Name))
            {
                Pid = Entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &Entry));
    }
    CloseHandle(hSnap);
    return Pid;
}

static void TestSystemProcess(PCWSTR Name)
{
    HANDLE hProcess, hToken;
    DWORD Pid = FindProcess(Name);

    if (!Pid)
    {
        skip("%ls not found\n", Name);
        return;
    }
    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
    if (!hProcess)
    {
        skip("OpenProcess(%ls) failed %lu\n", Name, GetLastError());
        return;
    }
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken))
    {
        DWORD Rid = GetTokenIntegrityRid(hToken);
        ok(Rid == SECURITY_MANDATORY_SYSTEM_RID, "%ls integrity 0x%lx\n", Name, Rid);
        CloseHandle(hToken);
    }
    else
    {
        skip("OpenProcessToken(%ls) failed %lu\n", Name, GetLastError());
    }
    CloseHandle(hProcess);
}

static void TestServiceAccount(PCWSTR Account)
{
    HANDLE hToken;

    if (!LogonUserW(Account, L"NT AUTHORITY", NULL, LOGON32_LOGON_SERVICE,
                    LOGON32_PROVIDER_DEFAULT, &hToken))
    {
        skip("LogonUserW(%ls) failed %lu\n", Account, GetLastError());
        return;
    }
    ok(GetTokenIntegrityRid(hToken) == SECURITY_MANDATORY_SYSTEM_RID, "%ls is not System\n", Account);
    CloseHandle(hToken);
}

START_TEST(TokenIntegrity)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    PSID AdminsSid = NULL;
    BOOL IsAdmin = FALSE;
    TOKEN_ELEVATION Elevation = { 0 };
    TOKEN_ELEVATION_TYPE ElevationType = 0;
    HANDLE hToken;
    DWORD Rid, cb;

    ok(AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                0, 0, 0, 0, 0, 0, &AdminsSid), "AllocateAndInitializeSid failed\n");
    ok(CheckTokenMembership(NULL, AdminsSid, &IsAdmin), "CheckTokenMembership failed %lu\n", GetLastError());

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        ok(0, "OpenProcessToken failed %lu\n", GetLastError());
        FreeSid(AdminsSid);
        return;
    }

    Rid = GetTokenIntegrityRid(hToken);
    ok(Rid == (IsAdmin ? SECURITY_MANDATORY_HIGH_RID : SECURITY_MANDATORY_MEDIUM_RID),
       "integrity 0x%lx, admin %d\n", Rid, IsAdmin);

    ok(GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cb),
       "TokenElevation failed %lu\n", GetLastError());
    ok(!!Elevation.TokenIsElevated == !!IsAdmin, "elevated %lu, admin %d\n", Elevation.TokenIsElevated, IsAdmin);

    ok(GetTokenInformation(hToken, TokenElevationType, &ElevationType, sizeof(ElevationType), &cb),
       "TokenElevationType failed %lu\n", GetLastError());
    ok(ElevationType == TokenElevationTypeDefault, "elevation type %d\n", ElevationType);

    CloseHandle(hToken);
    FreeSid(AdminsSid);

    TestSystemProcess(L"winlogon.exe");
    TestSystemProcess(L"services.exe");
    TestSystemProcess(L"lsass.exe");
    TestServiceAccount(L"LocalService");
    TestServiceAccount(L"NetworkService");
}
