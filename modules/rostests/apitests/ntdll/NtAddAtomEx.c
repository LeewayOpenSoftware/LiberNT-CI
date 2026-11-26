/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NtAddAtomEx parameter, flag, lifetime and concurrency behavior
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#ifndef ATOM_FLAG_GLOBAL
#define ATOM_FLAG_NONE   0x0
#define ATOM_FLAG_GLOBAL 0x2
#endif

typedef NTSTATUS (NTAPI *PNT_ADD_ATOM_EX)(PCWSTR, ULONG, PRTL_ATOM, ULONG);

typedef struct _ATOM_BASIC_BUFFER
{
    ATOM_BASIC_INFORMATION Information;
    WCHAR ExtraName[RTL_MAXIMUM_ATOM_LENGTH];
} ATOM_BASIC_BUFFER, *PATOM_BASIC_BUFFER;

typedef struct _ATOM_THREAD_CONTEXT
{
    PNT_ADD_ATOM_EX AddAtomEx;
    PCWSTR Name;
    ULONG NameLength;
    HANDLE StartEvent;
    RTL_ATOM ExpectedAtom;
    ULONG Flags;
    ULONG Iterations;
    LONG SuccessCount;
    LONG MismatchCount;
    NTSTATUS FirstFailure;
} ATOM_THREAD_CONTEXT, *PATOM_THREAD_CONTEXT;

static VOID
MakeName(
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _In_ PCWSTR Suffix)
{
    StringCchPrintfW(Buffer,
                     Count,
                     L"NP01_%08lx_%08lx_%s",
                     GetCurrentProcessId(),
                     GetTickCount(),
                     Suffix);
}

static NTSTATUS
QueryBasic(
    _In_ RTL_ATOM Atom,
    _Out_ PATOM_BASIC_BUFFER Buffer)
{
    ULONG ReturnLength = 0;

    RtlZeroMemory(Buffer, sizeof(*Buffer));
    return NtQueryInformationAtom(Atom,
                                  AtomBasicInformation,
                                  Buffer,
                                  sizeof(*Buffer),
                                  &ReturnLength);
}

static DWORD WINAPI
AddThread(
    _In_ PVOID Parameter)
{
    PATOM_THREAD_CONTEXT Context = Parameter;
    ULONG Index;

    WaitForSingleObject(Context->StartEvent, INFINITE);
    for (Index = 0; Index < Context->Iterations; Index++)
    {
        RTL_ATOM Atom = 0;
        NTSTATUS Status;

        Status = Context->AddAtomEx(Context->Name,
                                    Context->NameLength,
                                    &Atom,
                                    Context->Flags);
        if (!NT_SUCCESS(Status))
        {
            InterlockedCompareExchange((PLONG)&Context->FirstFailure,
                                       Status,
                                       STATUS_SUCCESS);
            break;
        }

        InterlockedIncrement(&Context->SuccessCount);
        if (Atom != Context->ExpectedAtom)
            InterlockedIncrement(&Context->MismatchCount);
    }

    return 0;
}

static VOID
TestInvalidParameters(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    static const ULONG InvalidFlags[] =
    {
        0x00000001,
        0x00000004,
        0x00000008,
        0x00010000,
        0x80000000,
        0xFFFFFFFF
    };
    WCHAR Name[64];
    WCHAR LongName[257];
    RTL_ATOM Atom;
    NTSTATUS Status;
    ULONG Index;

    MakeName(Name, ARRAYSIZE(Name), L"invalid");
    for (Index = 0; Index < ARRAYSIZE(InvalidFlags); Index++)
    {
        Atom = 0xABCD;
        Status = AddAtomEx(Name,
                           (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                           &Atom,
                           InvalidFlags[Index]);
        trace("flag %#lx -> %#lx, atom %#x\n",
              InvalidFlags[Index], Status, Atom);
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
        ok_hex(Atom, 0xABCD);
        if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);
    }

    Atom = 0xABCD;
    Status = AddAtomEx(NULL, 0, &Atom, ATOM_FLAG_NONE);
    trace("NULL name, zero length -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    ok_hex(Atom, 0xABCD);

    Atom = 0xABCD;
    Status = AddAtomEx(L"", 0, &Atom, ATOM_FLAG_NONE);
    trace("empty name -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_OBJECT_NAME_INVALID);
    ok_hex(Atom, 0xABCD);

    Atom = 0xABCD;
    Status = AddAtomEx((PCWSTR)(ULONG_PTR)0x10000,
                       sizeof(WCHAR),
                       &Atom,
                       ATOM_FLAG_NONE);
    trace("invalid input -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
    ok_hex(Atom, 0xABCD);

    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       (PRTL_ATOM)(ULONG_PTR)0x10000,
                       ATOM_FLAG_NONE);
    trace("invalid output -> %#lx\n", Status);
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
    Status = NtFindAtom(Name,
                        (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                        &Atom);
    ok_ntstatus(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);

    for (Index = 0; Index < ARRAYSIZE(LongName) - 1; Index++)
        LongName[Index] = L'A';
    LongName[ARRAYSIZE(LongName) - 1] = UNICODE_NULL;

    Atom = 0;
    Status = AddAtomEx(LongName, 510, &Atom, ATOM_FLAG_NONE);
    trace("510-byte name -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);

    Atom = 0xABCD;
    Status = AddAtomEx(LongName, 512, &Atom, ATOM_FLAG_NONE);
    trace("512-byte name -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    ok_hex(Atom, 0xABCD);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);
}

static VOID
TestExtendedValidation(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    ATOM_BASIC_BUFFER Basic;
    WCHAR LongName[257];
    WCHAR Name[64];
    RTL_ATOM Atom;
    NTSTATUS Status;
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(LongName) - 1; Index++)
        LongName[Index] = L'A';
    LongName[ARRAYSIZE(LongName) - 1] = UNICODE_NULL;

    Atom = 0;
    Status = AddAtomEx(LongName, 509, &Atom, ATOM_FLAG_NONE);
    trace("509-byte name -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = QueryBasic(Atom, &Basic);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok_hex(Basic.Information.NameLength, 508);
        NtDeleteAtom(Atom);
    }

    Atom = 0xABCD;
    Status = AddAtomEx(LongName, 511, &Atom, ATOM_FLAG_NONE);
    trace("511-byte name -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    ok_hex(Atom, 0xABCD);

    Atom = 0xABCD;
    Status = AddAtomEx((PCWSTR)(ULONG_PTR)0x10001,
                       sizeof(WCHAR),
                       &Atom,
                       ATOM_FLAG_NONE);
    trace("misaligned input -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_DATATYPE_MISALIGNMENT);
    ok_hex(Atom, 0xABCD);

    Atom = 0xABCD;
    Status = AddAtomEx(L"#49152", 7 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#49152 -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    ok_hex(Atom, 0xABCD);

    Atom = 0;
    Status = AddAtomEx(L"#0007", 6 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#0007 -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Atom, 7);

    Atom = 0;
    Status = AddAtomEx(L"#123x", 6 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#123x -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Atom >= 0xC000, "#123x must be a string atom, got %#x\n", Atom);
        NtDeleteAtom(Atom);
    }

    /* Win11-pending rows: record, do not pin. */
    Atom = 0;
    Status = AddAtomEx(L"#0", 3 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#0 -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    Atom = 0;
    Status = AddAtomEx(L"#65537", 7 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#65537 -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    Atom = 0;
    Status = AddAtomEx(L"#-1", 4 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#-1 -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    Atom = 0;
    Status = AddAtomEx(L"#+1", 4 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#+1 -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    /* Pointer-form integer atoms are only reachable from kernel mode. */
    Atom = 0xABCD;
    Status = AddAtomEx((PCWSTR)(ULONG_PTR)123, 0, &Atom, ATOM_FLAG_NONE);
    trace("pointer-form from user mode -> %#lx, atom %#x\n", Status, Atom);
    ok_hex(Atom, 0xABCD);

    MakeName(Name, ARRAYSIZE(Name), L"cleanup");
    Status = NtFindAtom(Name, (ULONG)(wcslen(Name) * sizeof(WCHAR)), &Atom);
    ok_ntstatus(Status, STATUS_OBJECT_NAME_NOT_FOUND);
}

static VOID
TestFlagsAndLifetime(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    ATOM_BASIC_BUFFER Basic;
    WCHAR Name[64];
    WCHAR UpperName[64];
    WCHAR OddName[3] = { 0xE100, 0xE101, UNICODE_NULL };
    RTL_ATOM Atom1, Atom2;
    NTSTATUS Status;
    SIZE_T Index;

    MakeName(Name, ARRAYSIZE(Name), L"ordinary");
    StringCchCopyW(UpperName, ARRAYSIZE(UpperName), Name);
    for (Index = 0; UpperName[Index]; Index++)
        UpperName[Index] = RtlUpcaseUnicodeChar(UpperName[Index]);

    Atom1 = Atom2 = 0;
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       &Atom1,
                       ATOM_FLAG_NONE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = AddAtomEx(UpperName,
                       (ULONG)(wcslen(UpperName) * sizeof(WCHAR)),
                       &Atom2,
                       ATOM_FLAG_GLOBAL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Atom2, Atom1);
    if (Atom1)
    {
        Status = QueryBasic(Atom1, &Basic);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            trace("0-then-2 flags %#x usage %u name %.*S\n",
                  Basic.Information.Flags,
                  Basic.Information.UsageCount,
                  Basic.Information.NameLength / sizeof(WCHAR),
                  Basic.Information.Name);
            ok_hex(Basic.Information.Flags & ATOM_FLAG_GLOBAL, 0);
            ok_hex(Basic.Information.UsageCount, 2);
        }
        NtDeleteAtom(Atom1);
        NtDeleteAtom(Atom1);
    }

    MakeName(Name, ARRAYSIZE(Name), L"global");
    Atom1 = Atom2 = 0;
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       &Atom1,
                       ATOM_FLAG_GLOBAL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       &Atom2,
                       ATOM_FLAG_NONE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Atom2, Atom1);
    if (Atom1)
    {
        Status = QueryBasic(Atom1, &Basic);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            trace("2-then-0 flags %#x usage %u\n",
                  Basic.Information.Flags,
                  Basic.Information.UsageCount);
            ok_hex(Basic.Information.Flags & ATOM_FLAG_GLOBAL,
                   ATOM_FLAG_GLOBAL);
            ok_hex(Basic.Information.UsageCount, 2);
        }
        NtDeleteAtom(Atom1);
        NtDeleteAtom(Atom1);
    }

    Atom1 = 0;
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       NULL,
                       ATOM_FLAG_NONE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtFindAtom(Name,
                        (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                        &Atom1);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom1);

    Atom1 = 0;
    Status = AddAtomEx(OddName, 3, &Atom1, ATOM_FLAG_NONE);
    trace("odd byte length -> %#lx, atom %#x\n", Status, Atom1);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = QueryBasic(Atom1, &Basic);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_hex(Basic.Information.NameLength, sizeof(WCHAR));
            ok_hex(Basic.Information.Name[0], OddName[0]);
        }
        NtDeleteAtom(Atom1);
    }

    Atom1 = 0;
    Status = AddAtomEx(L"#123", 4 * sizeof(WCHAR), &Atom1, ATOM_FLAG_GLOBAL);
    trace("integer atom -> %#lx, atom %#x\n", Status, Atom1);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Atom1, 123);
}

static VOID
TestConcurrency(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    enum { ThreadCount = 4, Iterations = 64 };
    ATOM_THREAD_CONTEXT Contexts[ThreadCount];
    HANDLE Threads[ThreadCount];
    ATOM_BASIC_BUFFER Basic;
    WCHAR Name[64];
    HANDLE StartEvent;
    RTL_ATOM Atom = 0;
    NTSTATUS Status;
    ULONG Index, Successes = 0;

    MakeName(Name, ARRAYSIZE(Name), L"concurrent");
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       &Atom,
                       ATOM_FLAG_GLOBAL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    StartEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(StartEvent != NULL, "CreateEvent failed: %lu\n", GetLastError());
    if (!StartEvent)
    {
        NtDeleteAtom(Atom);
        return;
    }

    RtlZeroMemory(Contexts, sizeof(Contexts));
    RtlZeroMemory(Threads, sizeof(Threads));
    for (Index = 0; Index < ThreadCount; Index++)
    {
        Contexts[Index].AddAtomEx = AddAtomEx;
        Contexts[Index].Name = Name;
        Contexts[Index].NameLength = (ULONG)(wcslen(Name) * sizeof(WCHAR));
        Contexts[Index].StartEvent = StartEvent;
        Contexts[Index].ExpectedAtom = Atom;
        Contexts[Index].Flags = (Index & 1) ? ATOM_FLAG_GLOBAL : ATOM_FLAG_NONE;
        Contexts[Index].Iterations = Iterations;
        Contexts[Index].FirstFailure = STATUS_SUCCESS;
        Threads[Index] = CreateThread(NULL, 0, AddThread, &Contexts[Index], 0, NULL);
        ok(Threads[Index] != NULL, "CreateThread %lu failed: %lu\n", Index, GetLastError());
    }

    SetEvent(StartEvent);
    for (Index = 0; Index < ThreadCount; Index++)
    {
        if (Threads[Index]) WaitForSingleObject(Threads[Index], INFINITE);
    }
    for (Index = 0; Index < ThreadCount; Index++)
    {
        if (Threads[Index]) CloseHandle(Threads[Index]);
        ok_ntstatus(Contexts[Index].FirstFailure, STATUS_SUCCESS);
        ok_hex(Contexts[Index].MismatchCount, 0);
        Successes += Contexts[Index].SuccessCount;
    }
    CloseHandle(StartEvent);

    Status = QueryBasic(Atom, &Basic);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        trace("concurrent flags %#x usage %u successes %lu\n",
              Basic.Information.Flags,
              Basic.Information.UsageCount,
              Successes);
        ok_hex(Basic.Information.Flags & ATOM_FLAG_GLOBAL, ATOM_FLAG_GLOBAL);
        ok_hex(Basic.Information.UsageCount, Successes + 1);
    }

    for (Index = 0; Index < Successes + 1; Index++)
        NtDeleteAtom(Atom);
}

static VOID
TestExhaustion(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    enum { MaximumAttempts = 0x4010 };
    WCHAR Enabled[2];
    WCHAR Prefix[48];
    WCHAR Name[64];
    PRTL_ATOM Atoms;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Attempt, Count = 0;

    if (!GetEnvironmentVariableW(L"NP01_ATOM_EXHAUSTION",
                                 Enabled,
                                 ARRAYSIZE(Enabled)))
    {
        skip("atom exhaustion disabled; set NP01_ATOM_EXHAUSTION=1 in a throwaway VM\n");
        return;
    }

    Atoms = HeapAlloc(GetProcessHeap(),
                      HEAP_ZERO_MEMORY,
                      MaximumAttempts * sizeof(*Atoms));
    ok(Atoms != NULL, "atom array allocation failed: %lu\n", GetLastError());
    if (!Atoms) return;

    MakeName(Prefix, ARRAYSIZE(Prefix), L"exhaust");
    for (Attempt = 0; Attempt < MaximumAttempts; Attempt++)
    {
        RTL_ATOM Atom = 0;

        StringCchPrintfW(Name, ARRAYSIZE(Name), L"%s_%04lx", Prefix, Attempt);
        Status = AddAtomEx(Name,
                           (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                           &Atom,
                           ATOM_FLAG_NONE);
        if (!NT_SUCCESS(Status)) break;

        ok(Atom >= 0xC000,
           "string atom %#x fell below the string range at attempt %lu\n",
           Atom,
           Attempt);
        Atoms[Count++] = Atom;
    }

    trace("atom exhaustion stopped after %lu additions with status %#lx\n",
          Count,
          Status);
    ok(Count <= 0x4000, "allocated %lu string atoms\n", Count);
    ok(!NT_SUCCESS(Status), "atom table did not report exhaustion\n");

    while (Count) NtDeleteAtom(Atoms[--Count]);
    HeapFree(GetProcessHeap(), 0, Atoms);
}

START_TEST(NtAddAtomEx)
{
    PNT_ADD_ATOM_EX AddAtomEx;

    AddAtomEx = (PNT_ADD_ATOM_EX)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtAddAtomEx");
    if (!AddAtomEx)
    {
        skip("NtAddAtomEx unavailable\n");
        return;
    }

    TestInvalidParameters(AddAtomEx);
    TestExtendedValidation(AddAtomEx);
    TestFlagsAndLifetime(AddAtomEx);
    TestConcurrency(AddAtomEx);
    TestExhaustion(AddAtomEx);
}
