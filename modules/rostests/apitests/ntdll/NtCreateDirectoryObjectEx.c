/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win11/Orosi comparison tests for NtCreateDirectoryObjectEx
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_NtCreateDirectoryObjectEx)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG);
static FN_NtCreateDirectoryObjectEx pNtCreateDirectoryObjectEx;

static WCHAR BnoPath[64];
static LONG Unique;

/* The race threads call this concurrently: the counter must be atomic or two creates share a name. */
static
VOID
MakeName(PWSTR Buffer, SIZE_T Count, PCWSTR Tag)
{
    StringCchPrintfW(Buffer, Count, L"%s\\npdx-%lu-%lu-%s", BnoPath,
                     HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess),
                     (ULONG)InterlockedIncrement(&Unique), Tag);
}

static
NTSTATUS
CreateDir(BOOLEAN Ex, PCWSTR Tag, ULONG Attributes, HANDLE Shadow, ULONG Flags, PHANDLE Handle)
{
    WCHAR Buffer[128];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;

    MakeName(Buffer, _countof(Buffer), Tag);
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | Attributes, NULL, NULL);
    *Handle = NULL;
    if (Ex)
        return pNtCreateDirectoryObjectEx(Handle, DIRECTORY_ALL_ACCESS, &Oa, Shadow, Flags);
    return NtCreateDirectoryObject(Handle, DIRECTORY_ALL_ACCESS, &Oa);
}

static
VOID
CloseIf(HANDLE Handle)
{
    if (Handle)
        NtClose(Handle);
}

/* The plain call (no shadow, no flags) must match NtCreateDirectoryObject. */
static
VOID
TestPlainEquivalence(VOID)
{
    HANDLE H1, H2;
    NTSTATUS S1, S2;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;

    S1 = CreateDir(FALSE, L"plain", 0, NULL, 0, &H1);
    S2 = CreateDir(TRUE, L"plain", 0, NULL, 0, &H2);
    ok_hex(S1, STATUS_SUCCESS);
    ok_hex(S2, S1);
    CloseIf(H1);
    CloseIf(H2);

    /* Bad output pointer */
    RtlInitUnicodeString(&Name, L"\\npdx-never");
    InitializeObjectAttributes(&Oa, &Name, 0, NULL, NULL);
    S1 = NtCreateDirectoryObject((PHANDLE)(ULONG_PTR)1, DIRECTORY_ALL_ACCESS, &Oa);
    S2 = pNtCreateDirectoryObjectEx((PHANDLE)(ULONG_PTR)1, DIRECTORY_ALL_ACCESS, &Oa, NULL, 0);
    trace("PROBE badhandleptr legacy=0x%lx ex=0x%lx\n", S1, S2);
    ok_hex(S2, S1);

    /* NULL object attributes */
    S1 = NtCreateDirectoryObject(&H1, DIRECTORY_ALL_ACCESS, NULL);
    S2 = pNtCreateDirectoryObjectEx(&H2, DIRECTORY_ALL_ACCESS, NULL, NULL, 0);
    trace("PROBE nulloa legacy=0x%lx ex=0x%lx\n", S1, S2);
    ok_hex(S2, S1);
    if (NT_SUCCESS(S1)) CloseIf(H1);
    if (NT_SUCCESS(S2)) CloseIf(H2);
}

/* Each Flags bit on its own: Win11 statuses are recorded, not assumed. */
static
VOID
TestFlags(VOID)
{
    ULONG Bit;
    HANDLE H;
    NTSTATUS Status;

    for (Bit = 0; Bit < 32; Bit++)
    {
        Status = CreateDir(TRUE, L"flag", 0, NULL, 1UL << Bit, &H);
        trace("PROBE flags 0x%08lx -> 0x%lx\n", 1UL << Bit, Status);
        CloseIf(H);
    }
}

static
VOID
TestShadowHandleChecks(VOID)
{
    HANDLE Event = NULL, Shadow = NULL, Weak = NULL, H;
    NTSTATUS Status;

    /* Not a directory */
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = CreateDir(TRUE, L"evshadow", 0, Event, 0, &H);
    trace("PROBE shadow=event -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "event as shadow accepted: 0x%lx\n", Status);
    CloseIf(H);

    /* Bogus handle */
    Status = CreateDir(TRUE, L"badshadow", 0, (HANDLE)(ULONG_PTR)0x7ffc, 0, &H);
    trace("PROBE shadow=bogus -> 0x%lx\n", Status);
    ok(!NT_SUCCESS(Status), "bogus shadow accepted: 0x%lx\n", Status);
    CloseIf(H);

    /* Directory handle without DIRECTORY_TRAVERSE */
    Status = CreateDir(FALSE, L"shadowsrc", 0, NULL, 0, &Shadow);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtDuplicateObject(NtCurrentProcess(), Shadow, NtCurrentProcess(), &Weak,
                                   DIRECTORY_QUERY, 0, 0);
        ok_hex(Status, STATUS_SUCCESS);
        Status = CreateDir(TRUE, L"weakshadow", 0, Weak, 0, &H);
        trace("PROBE shadow=dir(QUERY only) -> 0x%lx\n", Status);
        CloseIf(H);
    }
    CloseIf(Weak);
    CloseIf(Shadow);
    CloseIf(Event);
}

static
NTSTATUS
OpenEventIn(HANDLE Root, PCWSTR Leaf, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;

    RtlInitUnicodeString(&Name, Leaf);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, Root, NULL);
    *Handle = NULL;
    return NtOpenEvent(Handle, SYNCHRONIZE, &Oa);
}

/* A miss in the new directory falls back to the shadow; a local hit wins. */
static
VOID
TestShadowLookup(VOID)
{
    HANDLE Shadow = NULL, Dir = NULL, InShadow = NULL, Opened = NULL, Local = NULL, Local2 = NULL, H = NULL;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS Status;
    OBJECT_BASIC_INFORMATION Info;

    Status = CreateDir(FALSE, L"shadow", 0, NULL, 0, &Shadow);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    RtlInitUnicodeString(&Name, L"only-in-shadow");
    InitializeObjectAttributes(&Oa, &Name, 0, Shadow, NULL);
    Status = NtCreateEvent(&InShadow, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    RtlInitUnicodeString(&Name, L"both");
    Status = NtCreateEvent(&H, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);

    Status = CreateDir(TRUE, L"child", 0, Shadow, 0, &Dir);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&Name, L"both");
        InitializeObjectAttributes(&Oa, &Name, 0, Dir, NULL);
        Status = NtCreateEvent(&Local, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
        trace("PROBE create 'both' in child -> 0x%lx\n", Status);

        Status = OpenEventIn(Dir, L"only-in-shadow", &Opened);
        trace("PROBE open shadow-only name via child -> 0x%lx\n", Status);
        ok_hex(Status, STATUS_SUCCESS);
        CloseIf(Opened);

        Status = OpenEventIn(Dir, L"missing-everywhere", &Opened);
        ok_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
        CloseIf(Opened);

        /* P3: inserts do not fall through; Win11 decides local create vs collision. */
        RtlInitUnicodeString(&Name, L"only-in-shadow");
        InitializeObjectAttributes(&Oa, &Name, 0, Dir, NULL);
        Status = NtCreateEvent(&Local2, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
        trace("PROBE create shadow-only name in child -> 0x%lx\n", Status);
        CloseIf(Local2);

        /* The shadow reference must not keep the child alive after close. */
        Status = NtQueryObject(Shadow, ObjectBasicInformation, &Info, sizeof(Info), NULL);
        ok_hex(Status, STATUS_SUCCESS);
        trace("PROBE shadow handles=%lu pointers=%lu with child open\n", Info.HandleCount, Info.PointerCount);
        CloseIf(Local);
        NtClose(Dir);
        Status = NtQueryObject(Shadow, ObjectBasicInformation, &Info, sizeof(Info), NULL);
        trace("PROBE shadow handles=%lu pointers=%lu after child close\n", Info.HandleCount, Info.PointerCount);
    }
    CloseIf(H);
    CloseIf(InShadow);
    NtClose(Shadow);
}

static
VOID
TestCollision(VOID)
{
    HANDLE H1 = NULL, H2 = NULL;
    WCHAR Buffer[128];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS Status;

    MakeName(Buffer, _countof(Buffer), L"coll");
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = pNtCreateDirectoryObjectEx(&H1, DIRECTORY_ALL_ACCESS, &Oa, NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
    Status = pNtCreateDirectoryObjectEx(&H2, DIRECTORY_ALL_ACCESS, &Oa, NULL, 0);
    ok_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    Oa.Attributes |= OBJ_OPENIF;
    Status = pNtCreateDirectoryObjectEx(&H2, DIRECTORY_ALL_ACCESS, &Oa, NULL, 0);
    ok_hex(Status, STATUS_OBJECT_NAME_EXISTS);
    CloseIf(H2);
    CloseIf(H1);
}

typedef struct _RACE { HANDLE Shadow; LONG Failures; } RACE;

static
DWORD
WINAPI
RaceThread(PVOID Param)
{
    RACE *Race = Param;
    HANDLE Dir, Ev;
    ULONG i;

    for (i = 0; i < 200; i++)
    {
        NTSTATUS Status = CreateDir(TRUE, L"race", 0, Race->Shadow, 0, &Dir);
        if (!NT_SUCCESS(Status))
        {
            trace("race create %lu -> 0x%lx\n", i, Status);
            InterlockedIncrement(&Race->Failures);
            continue;
        }
        if (NT_SUCCESS(OpenEventIn(Dir, L"race-ev", &Ev)))
            NtClose(Ev);
        NtClose(Dir);
    }
    return 0;
}

static
VOID
TestConcurrency(VOID)
{
    RACE Race = { NULL, 0 };
    HANDLE Threads[4], Ev = NULL;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    ULONG i;

    if (!NT_SUCCESS(CreateDir(FALSE, L"raceshadow", 0, NULL, 0, &Race.Shadow)))
        return;
    RtlInitUnicodeString(&Name, L"race-ev");
    InitializeObjectAttributes(&Oa, &Name, 0, Race.Shadow, NULL);
    NtCreateEvent(&Ev, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    for (i = 0; i < _countof(Threads); i++)
        Threads[i] = CreateThread(NULL, 0, RaceThread, &Race, 0, NULL);
    WaitForMultipleObjects(_countof(Threads), Threads, TRUE, 60000);
    for (i = 0; i < _countof(Threads); i++)
        CloseHandle(Threads[i]);
    ok(Race.Failures == 0, "%ld creates with a shared shadow failed\n", Race.Failures);
    CloseIf(Ev);
    NtClose(Race.Shadow);
}

/* P2: C -> A -> B, the name lives only in B. Win11 decides whether shadows chain. */
static
VOID
TestShadowChain(VOID)
{
    HANDLE A = NULL, B = NULL, C = NULL, Ev = NULL, H = NULL;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    NTSTATUS Status;

    if (!NT_SUCCESS(CreateDir(FALSE, L"chainB", 0, NULL, 0, &B)))
        return;
    RtlInitUnicodeString(&Name, L"only-in-b");
    InitializeObjectAttributes(&Oa, &Name, 0, B, NULL);
    Status = NtCreateEvent(&Ev, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);
    Status = CreateDir(TRUE, L"chainA", 0, B, 0, &A);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = CreateDir(TRUE, L"chainC", 0, A, 0, &C);
        trace("PROBE create C with shadow A (itself shadowed) -> 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
        {
            Status = OpenEventIn(C, L"only-in-b", &H);
            trace("PROBE open B-only name via C->A->B -> 0x%lx\n", Status);
            CloseIf(H);
        }
    }
    CloseIf(C);
    CloseIf(A);
    CloseIf(Ev);
    NtClose(B);
}

/* D6: the child keeps its own shadow reference; the shadow's creator handle can go away. */
static
VOID
TestShadowLifetime(VOID)
{
    HANDLE Shadow = NULL, Dir = NULL, Ev = NULL, H;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    WCHAR Buffer[128];
    NTSTATUS Status;

    /* Unnamed, so closing the last handle deletes it as far as names go. */
    InitializeObjectAttributes(&Oa, NULL, 0, NULL, NULL);
    Status = NtCreateDirectoryObject(&Shadow, DIRECTORY_ALL_ACCESS, &Oa);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    RtlInitUnicodeString(&Name, L"lifetime-ev");
    InitializeObjectAttributes(&Oa, &Name, 0, Shadow, NULL);
    Status = NtCreateEvent(&Ev, EVENT_ALL_ACCESS, &Oa, NotificationEvent, FALSE);
    ok_hex(Status, STATUS_SUCCESS);

    MakeName(Buffer, _countof(Buffer), L"lifetime");
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = pNtCreateDirectoryObjectEx(&Dir, DIRECTORY_ALL_ACCESS, &Oa, Shadow, 0);
    ok_hex(Status, STATUS_SUCCESS);
    NtClose(Shadow);
    if (NT_SUCCESS(Status))
    {
        Status = OpenEventIn(Dir, L"lifetime-ev", &H);
        trace("PROBE open via child after shadow handle closed -> 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
            NtClose(H);
        NtClose(Dir);
    }
    CloseIf(Ev);
}

START_TEST(NtCreateDirectoryObjectEx)
{
    ULONG Session = NtCurrentPeb()->SessionId;

    pNtCreateDirectoryObjectEx = (FN_NtCreateDirectoryObjectEx)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateDirectoryObjectEx");
    if (!pNtCreateDirectoryObjectEx)
    {
        skip("NtCreateDirectoryObjectEx is not exported\n");
        return;
    }
    if (Session == 0)
        StringCchCopyW(BnoPath, _countof(BnoPath), L"\\BaseNamedObjects");
    else
        StringCchPrintfW(BnoPath, _countof(BnoPath), L"\\Sessions\\%lu\\BaseNamedObjects", Session);

    TestPlainEquivalence();
    TestFlags();
    TestShadowHandleChecks();
    TestShadowLookup();
    TestCollision();
    TestShadowChain();
    TestShadowLifetime();
    TestConcurrency();
}
