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
    static const ULONG BadFlagCombos[] =
    {
        0xFFFFFFFF,
        0x80000000,
        0x00010002,                 /* GLOBAL set plus an undefined bit */
        ATOM_FLAG_GLOBAL | 0x00000001
    };
    WCHAR Name[64];
    WCHAR LongName[257];
    RTL_ATOM Atom;
    NTSTATUS Status;
    ULONG Index;
    ULONG Flag;

    MakeName(Name, ARRAYSIZE(Name), L"invalid");

    /* Sweep every single flag bit across the 32-bit space. Only
       ATOM_FLAG_GLOBAL (bit 1) is defined, so every other bit must be
       rejected with STATUS_INVALID_PARAMETER before any name work (Win11
       baseline: an unknown flag -> 0xC000000D first). A bit that Win11 turns
       out to accept would surface here as a failed assertion. */
    for (Index = 0; Index < 32; Index++)
    {
        Flag = 1UL << Index;
        if (Flag == ATOM_FLAG_GLOBAL) continue;
        Atom = 0xABCD;
        Status = AddAtomEx(Name,
                           (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                           &Atom,
                           Flag);
        ok(Status == STATUS_INVALID_PARAMETER,
           "flag bit %#lx: expected STATUS_INVALID_PARAMETER, got %#lx\n",
           Flag, Status);
        ok_hex(Atom, 0xABCD);
        if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);
    }

    /* Multi-bit combinations, including GLOBAL together with an undefined bit. */
    for (Index = 0; Index < ARRAYSIZE(BadFlagCombos); Index++)
    {
        Atom = 0xABCD;
        Status = AddAtomEx(Name,
                           (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                           &Atom,
                           BadFlagCombos[Index]);
        trace("flag combo %#lx -> %#lx, atom %#x\n",
              BadFlagCombos[Index], Status, Atom);
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
TestCombinedValidationOrder(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    const ULONG InvalidFlag = 0x00000001;
    WCHAR LongName[257];
    WCHAR Name[64];
    RTL_ATOM Atom;
    NTSTATUS Status;
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(LongName) - 1; Index++)
        LongName[Index] = L'A';
    LongName[ARRAYSIZE(LongName) - 1] = UNICODE_NULL;

    /* Two faults at once: record which one wins, pin the precedence only after
       the Win11 run. Every combination must still fail and create no atom. */
    Atom = 0xABCD;
    Status = AddAtomEx(NULL, 0, &Atom, InvalidFlag);
    trace("invalid flag + NULL name -> %#lx, atom %#x (Win11 pending)\n",
          Status, Atom);
    ok(!NT_SUCCESS(Status),
       "invalid flag + NULL name unexpectedly succeeded: %#lx\n", Status);
    ok_hex(Atom, 0xABCD);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);

    Atom = 0xABCD;
    Status = AddAtomEx(LongName, 512, &Atom, InvalidFlag);
    trace("invalid flag + 512-byte name -> %#lx, atom %#x (Win11 pending)\n",
          Status, Atom);
    ok(!NT_SUCCESS(Status),
       "invalid flag + 512-byte name unexpectedly succeeded: %#lx\n", Status);
    ok_hex(Atom, 0xABCD);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);

    MakeName(Name, ARRAYSIZE(Name), L"orderbadout");
    Status = AddAtomEx(Name,
                       (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                       (PRTL_ATOM)(ULONG_PTR)0x10000,
                       InvalidFlag);
    trace("invalid flag + bad output -> %#lx (Win11 pending)\n", Status);
    ok(!NT_SUCCESS(Status),
       "invalid flag + bad output unexpectedly succeeded: %#lx\n", Status);
    Status = NtFindAtom(Name,
                        (ULONG)(wcslen(Name) * sizeof(WCHAR)),
                        &Atom);
    ok_ntstatus(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);
}

static VOID
TestExtendedValidation(
    _In_ PNT_ADD_ATOM_EX AddAtomEx)
{
    ATOM_BASIC_BUFFER Basic;
    WCHAR LongName[257];
    WCHAR Name[64];
    WCHAR EmbeddedNul[8];
    RTL_ATOM Atom;
    NTSTATUS Status;
    ULONG Index;
    static const ULONG OverLengths[] =
        { 0x00010002, 0x000101FE, 0xFFFFFFFE, 0xFFFFFFFF };

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

    /* T2: lengths above RTL_MAXIMUM_ATOM_LENGTH*sizeof(WCHAR) (0x1FE). The
       length gate rejects them before the name is touched, so no fault leaks
       out even for the wrap-prone 0xFFFFFFFE/0xFFFFFFFF -- platform independent. */
    for (Index = 0; Index < ARRAYSIZE(OverLengths); Index++)
    {
        Atom = 0xABCD;
        Status = AddAtomEx(LongName, OverLengths[Index], &Atom, ATOM_FLAG_NONE);
        trace("length %#lx -> %#lx, atom %#x\n",
              OverLengths[Index], Status, Atom);
        ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
        ok_hex(Atom, 0xABCD);
        if (NT_SUCCESS(Status)) NtDeleteAtom(Atom);
    }

    /* T2: odd (non-WCHAR-aligned) input address -> the alignment probe rejects it. */
    Atom = 0xABCD;
    Status = AddAtomEx((PCWSTR)(ULONG_PTR)0x20001,
                       sizeof(WCHAR),
                       &Atom,
                       ATOM_FLAG_NONE);
    trace("odd address 0x20001 -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_DATATYPE_MISALIGNMENT);
    ok_hex(Atom, 0xABCD);

    /* T2: embedded NUL. The capture copies the full declared length, so the
       stored name spans the NUL; whether the table keeps or truncates it is a
       Win11 question -- record, do not pin. */
    EmbeddedNul[0] = L'A';
    EmbeddedNul[1] = L'B';
    EmbeddedNul[2] = UNICODE_NULL;
    EmbeddedNul[3] = L'C';
    EmbeddedNul[4] = L'D';
    EmbeddedNul[5] = UNICODE_NULL;
    Atom = 0;
    Status = AddAtomEx(EmbeddedNul, 5 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("embedded NUL -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    /* T3: #49151 = 0xBFFF, the top valid integer atom (one below the 0xC000
       string-atom base whose neighbour #49152 fails above). */
    Atom = 0;
    Status = AddAtomEx(L"#49151", 7 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#49151 -> %#lx, atom %#x\n", Status, Atom);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Atom, 0xBFFF);

    /* T3: non-numeric tail and a bare '#' -- Win11-pending, record only. */
    Atom = 0;
    Status = AddAtomEx(L"#12a", 5 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("#12a -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    Atom = 0;
    Status = AddAtomEx(L"#", 2 * sizeof(WCHAR), &Atom, ATOM_FLAG_NONE);
    trace("bare # -> %#lx, atom %#x (Win11 pending)\n", Status, Atom);
    if (NT_SUCCESS(Status) && Atom >= 0xC000) NtDeleteAtom(Atom);

    /* Pointer-form integer atoms (MAKEINTATOM(123) == this cast) are only
       reachable from kernel mode; from user mode the value is left untouched. */
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
            trace("0-then-2 flags %#x usage %u name %S\n",
                  Basic.Information.Flags,
                  Basic.Information.UsageCount,
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
    WCHAR PreName[64];
    RTL_ATOM PreAtom = 0;
    NTSTATUS PreStatus;

    AddAtomEx = (PNT_ADD_ATOM_EX)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtAddAtomEx");
    if (!AddAtomEx)
    {
        skip("NtAddAtomEx unavailable\n");
        return;
    }

    /* Win11 ties the atom table to the window station: a process that has not
       loaded user32 is denied every valid add with 0xC0000022 (Pam mvzy's
       win11 baseline), while an invalid flag is still rejected first with
       0xC000000D. Orosi has no such callout and falls back to the kernel atom
       table (ntoskrnl/ex/atom.c). Record the pre-user32 status for the Win11
       diff (traced, never asserted -- it is exactly the platform split under
       study), then load user32 so the valid/order/lifetime/exhaustion rows
       below run with a window station attached on either platform. */
    MakeName(PreName, ARRAYSIZE(PreName), L"preuser32");
    PreStatus = AddAtomEx(PreName,
                          (ULONG)(wcslen(PreName) * sizeof(WCHAR)),
                          &PreAtom,
                          ATOM_FLAG_NONE);
    trace("pre-user32 add -> %#lx, atom %#x "
          "(Win11 expects 0xC0000022; Orosi falls back to the kernel table)\n",
          PreStatus, PreAtom);
    if (NT_SUCCESS(PreStatus))
        NtDeleteAtom(PreAtom);

    ok(LoadLibraryW(L"user32.dll") != NULL,
       "LoadLibraryW(user32.dll) failed: %lu (station-bound rows will differ)\n",
       GetLastError());

    TestInvalidParameters(AddAtomEx);
    TestCombinedValidationOrder(AddAtomEx);
    TestExtendedValidation(AddAtomEx);
    TestFlagsAndLifetime(AddAtomEx);
    TestConcurrency(AddAtomEx);
    TestExhaustion(AddAtomEx);
}
