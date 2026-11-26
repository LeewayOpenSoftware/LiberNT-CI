/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the capacity of the local string atom table
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Windows 11 26100 holds exactly 0x4000 unique local string atoms
 * (0xC000-0xFFFF), fails the next unique add with ERROR_NOT_ENOUGH_MEMORY
 * and reuses a freed value at once. The table is per process, so the test
 * fills it in a child process.
 */

#include "precomp.h"

#define ATOM_CAPACITY   0x4000
#define ATOM_TRY_LIMIT  0x5000
#define CHILD_WAIT_MS   120000

static ULONG Failures;

#define child_ok(Condition, ...) \
    do { if (!(Condition)) { Failures++; trace(__VA_ARGS__); } } while (0)

static
ATOM
AddUnique(
    _In_ ULONG Index)
{
    WCHAR Name[40];

    StringCbPrintfW(Name, sizeof(Name), L"OROSI_LOCAL_ATOM_%08lx", Index);
    SetLastError(0xDEADBEEF);
    return AddAtomW(Name);
}

static
VOID
RunChild(VOID)
{
    static BYTE Seen[ATOM_CAPACITY / 8];
    ATOM Atom, First = 0, Last = 0, Middle = 0;
    ULONG Count = 0, Index;
    WCHAR Name[40];
    DWORD Error = 0;

    for (Index = 0; Index < ATOM_TRY_LIMIT; Index++)
    {
        Atom = AddUnique(Index);
        if (Atom == 0)
        {
            Error = GetLastError();
            break;
        }

        child_ok(Atom >= 0xC000, "add %lu returned 0x%04x, outside 0xC000-0xFFFF\n", Index, Atom);
        if (Atom >= 0xC000)
        {
            ULONG Bit = Atom - 0xC000;

            child_ok(!(Seen[Bit / 8] & (1 << (Bit % 8))), "add %lu returned duplicate 0x%04x\n", Index, Atom);
            Seen[Bit / 8] |= (BYTE)(1 << (Bit % 8));
        }

        if (Count == 0)
            First = Atom;
        if (Count == ATOM_CAPACITY / 2)
            Middle = Atom;
        Last = Atom;
        Count++;
    }

    child_ok(Count == ATOM_CAPACITY, "%lu unique adds succeeded, expected %u\n", Count, ATOM_CAPACITY);
    child_ok(Error == ERROR_NOT_ENOUGH_MEMORY, "exhaustion error %lu, expected 8\n", Error);

    /* A further unique add keeps failing the same way */
    Atom = AddUnique(ATOM_TRY_LIMIT + 1);
    child_ok(Atom == 0 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
             "add past the limit: 0x%04x, error %lu\n", Atom, GetLastError());

    /* The ANSI path shares the table */
    SetLastError(0xDEADBEEF);
    Atom = AddAtomA("OROSI_LOCAL_ATOM_ANSI");
    child_ok(Atom == 0 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
             "AddAtomA past the limit: 0x%04x, error %lu\n", Atom, GetLastError());

    /* An existing name only takes a reference */
    SetLastError(0xDEADBEEF);
    Atom = AddAtomW(L"OROSI_LOCAL_ATOM_00000000");
    child_ok(Atom == First && GetLastError() == 0,
             "re-add of the first name: 0x%04x, error %lu (expected 0x%04x, 0)\n", Atom, GetLastError(), First);

    /* Integer atoms use no slot */
    Atom = AddAtomW(L"#123");
    child_ok(Atom == 123, "integer atom returned %u\n", Atom);

    /* Lookups still work at exhaustion */
    child_ok(FindAtomW(L"OROSI_LOCAL_ATOM_00000000") == First, "FindAtomW(first) failed\n");
    StringCbPrintfW(Name, sizeof(Name), L"OROSI_LOCAL_ATOM_%08lx", (ULONG)(ATOM_CAPACITY / 2));
    child_ok(FindAtomW(Name) == Middle, "FindAtomW(middle) failed\n");
    Name[0] = 0;
    child_ok(GetAtomNameW(Last, Name, ARRAYSIZE(Name)) != 0, "GetAtomNameW(last) failed: %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    child_ok(FindAtomW(L"OROSI_LOCAL_ATOM_NEVER") == 0 && GetLastError() == ERROR_FILE_NOT_FOUND,
             "FindAtomW(never added): error %lu\n", GetLastError());

    /* A freed value is handed out again at once */
    child_ok(DeleteAtom(Last) == 0, "DeleteAtom(0x%04x) failed\n", Last);
    Atom = AddUnique(ATOM_TRY_LIMIT + 2);
    child_ok(Atom == Last && GetLastError() == 0,
             "add after delete: 0x%04x, error %lu (expected 0x%04x, 0)\n", Atom, GetLastError(), Last);
    Atom = AddUnique(ATOM_TRY_LIMIT + 3);
    child_ok(Atom == 0 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
             "add after reuse: 0x%04x, error %lu\n", Atom, GetLastError());

    ExitProcess(Failures);
}

START_TEST(LocalAtomExhaustion)
{
    CHAR Path[MAX_PATH], Arguments[MAX_PATH * 2];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0xFFFFFFFF;
    char **Argv;
    int Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3 && !strcmp(Argv[2], "child"))
    {
        RunChild();
        return;
    }

    if (!GetModuleFileNameA(NULL, Path, ARRAYSIZE(Path)))
    {
        skip("GetModuleFileNameA failed: %lu\n", GetLastError());
        return;
    }

    StringCbPrintfA(Arguments, sizeof(Arguments), "\"%s\" LocalAtomExhaustion child", Path);
    if (!CreateProcessA(NULL, Arguments, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcess failed: %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);

    Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "child wait returned %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(ProcessInfo.hProcess, 1);
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    ok(ExitCode == 0, "child reported %lu failed checks (see the trace lines above)\n", ExitCode);
    CloseHandle(ProcessInfo.hProcess);
}
