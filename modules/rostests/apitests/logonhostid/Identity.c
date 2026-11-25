/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The logon UI host runs as SYSTEM on the Winlogon desktop, out of a user's reach
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <tlhelp32.h>

static DWORD
FindProcess(PCWSTR Name)
{
    HANDLE Snapshot;
    PROCESSENTRY32W Entry;
    DWORD Pid = 0;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;
    Entry.dwSize = sizeof(Entry);
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (_wcsicmp(Entry.szExeFile, Name) == 0)
            {
                Pid = Entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Pid;
}

static BOOL
IsAdministrator(void)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    PSID Admins = NULL;
    BOOL Member = FALSE;

    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &Admins))
    {
        if (!CheckTokenMembership(NULL, Admins, &Member))
            Member = FALSE;
        FreeSid(Admins);
    }
    return Member;
}

static void
CheckProcessClosed(PCWSTR Name, DWORD Pid)
{
    static const struct { DWORD Access; PCSTR Label; } Opens[] =
    {
        { PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, "query+read" },
        { PROCESS_TERMINATE, "terminate" },
    };
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Opens); i++)
    {
        HANDLE Process = OpenProcess(Opens[i].Access, FALSE, Pid);
        DWORD Error = GetLastError();

        ok(Process == NULL, "%ls pid %lu: %s opened from the user session\n", Name, Pid, Opens[i].Label);
        if (Process != NULL)
            CloseHandle(Process);
        else
            ok(Error == ERROR_ACCESS_DENIED, "%ls %s: error %lu (expected 5)\n", Name, Opens[i].Label, Error);
    }
}

START_TEST(Identity)
{
    WCHAR Value[8];
    DWORD Pid;
    HDESK Desktop;
    DWORD Error;

    if (IsAdministrator())
    {
        skip("running as an administrator; this is a standard-user check\n");
        return;
    }

    /* The host only lives while the logon UI is up: with LOGONHOSTID_LOCK set this test locks
     * its own session and waits for the host, so unlock by hand afterwards and read the output. */
    if (GetEnvironmentVariableW(L"LOGONHOSTID_LOCK", Value, ARRAYSIZE(Value)) != 0)
    {
        DWORD Started = GetTickCount();

        ok(LockWorkStation(), "LockWorkStation failed: %lu\n", GetLastError());
        while (FindProcess(L"logonuihost.exe") == 0 && GetTickCount() - Started < 30000)
            Sleep(500);
    }

    Pid = FindProcess(L"logonuihost.exe");
    if (Pid != 0)
        CheckProcessClosed(L"logonuihost.exe", Pid);
    else
        skip("logonuihost.exe is not running (set LOGONHOSTID_LOCK=1 to lock and wait for it)\n");

    Pid = FindProcess(L"winlogon.exe");
    ok(Pid != 0, "winlogon.exe not found\n");
    if (Pid != 0)
        CheckProcessClosed(L"winlogon.exe", Pid);

    Desktop = OpenDesktopW(L"Winlogon", 0, FALSE, DESKTOP_READOBJECTS);
    Error = GetLastError();
    ok(Desktop == NULL, "the Winlogon desktop opened from the user session\n");
    if (Desktop != NULL)
        CloseDesktop(Desktop);
    else
        ok(Error == ERROR_ACCESS_DENIED, "OpenDesktop(Winlogon): error %lu (expected 5)\n", Error);

    /* Control: the user's own desktop opens the same way. */
    Desktop = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS);
    ok(Desktop != NULL, "OpenDesktop(Default) failed: %lu\n", GetLastError());
    if (Desktop != NULL)
        CloseDesktop(Desktop);
}
