/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win11/Orosi comparison probe for NtQuerySystemInformation(SystemSessionProcessInformation)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define BIG_SIZE (4 * 1024 * 1024)

static ULONG OwnSession;
static BOOLEAN HaveTcb;

static
NTSTATUS
QuerySession(ULONG SessionId, PVOID Buffer, ULONG BufferSize, PULONG ReturnLength)
{
    SYSTEM_SESSION_PROCESS_INFORMATION Info;

    Info.SessionId = SessionId;
    Info.SizeOfBuf = BufferSize;
    Info.Buffer = Buffer;
    return NtQuerySystemInformation(SystemSessionProcessInformation, &Info, sizeof(Info), ReturnLength);
}

/* Walks a record chain; returns the record count, the count of records not in Session, and whether Pid is in it */
static
ULONG
WalkChain(PVOID Buffer, ULONG Size, ULONG Session, PULONG Foreign, HANDLE Pid, PBOOLEAN FoundPid)
{
    PSYSTEM_PROCESS_INFORMATION Spi = Buffer;
    ULONG Count = 0, Offset = 0;

    *Foreign = 0;
    *FoundPid = FALSE;
    for (;;)
    {
        if (Offset + sizeof(*Spi) > Size)
            break;
        Spi = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Buffer + Offset);
        Count++;
        if (Spi->SessionId != Session)
            (*Foreign)++;
        if (Spi->UniqueProcessId == Pid)
            *FoundPid = TRUE;
        if (Spi->NextEntryOffset == 0)
            break;
        Offset += Spi->NextEntryOffset;
    }
    return Count;
}

/* P1 + P9 + P10: own session, compared with class 5 filtered by SessionId */
static
VOID
TestOwnSession(PUCHAR Buffer)
{
    NTSTATUS Status;
    ULONG Ret = 0xDEADBEEF, Count, Foreign, Count5, InSession5 = 0, Foreign5, Offset = 0;
    BOOLEAN Found, Found5;
    PSYSTEM_PROCESS_INFORMATION Spi;
    HANDLE Pid = UlongToHandle(GetCurrentProcessId());

    Status = QuerySession(OwnSession, Buffer, BIG_SIZE, &Ret);
    trace("PROBE P1 own session %lu -> 0x%lx ret=%lu\n", OwnSession, Status, Ret);
    if (!NT_SUCCESS(Status))
    {
        skip("own-session query failed: 0x%lx\n", Status);
        return;
    }
    Count = WalkChain(Buffer, BIG_SIZE, OwnSession, &Foreign, Pid, &Found);
    trace("PROBE P1 records=%lu foreign=%lu ownpid=%d\n", Count, Foreign, Found);
    ok(Foreign == 0, "%lu records are not in session %lu\n", Foreign, OwnSession);
    ok(Found, "own process not listed\n");
    ok(Count > 0, "no records\n");

    Status = QuerySession(OwnSession, Buffer, BIG_SIZE, NULL);
    trace("PROBE P9 ReturnLength NULL -> 0x%lx\n", Status);

    Ret = 0;
    Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, BIG_SIZE, &Ret);
    trace("PROBE class 5 -> 0x%lx ret=%lu\n", Status, Ret);
    if (!NT_SUCCESS(Status))
        return;
    Count5 = WalkChain(Buffer, BIG_SIZE, OwnSession, &Foreign5, Pid, &Found5);
    for (;;)
    {
        Spi = (PSYSTEM_PROCESS_INFORMATION)(Buffer + Offset);
        if (Spi->SessionId == OwnSession)
            InSession5++;
        if (Spi->NextEntryOffset == 0)
            break;
        Offset += Spi->NextEntryOffset;
    }
    trace("PROBE P1 class 5: records=%lu in session %lu: %lu (class 53 gave %lu)\n",
          Count5, OwnSession, InSession5, Count);
}

/* P2 + P3: the outer length and alignment */
static
VOID
TestOuter(PUCHAR Buffer)
{
    static const ULONG Lengths[] = { 0, sizeof(SYSTEM_SESSION_PROCESS_INFORMATION) - 1,
                                     sizeof(SYSTEM_SESSION_PROCESS_INFORMATION) + 4 };
    UCHAR Raw[sizeof(SYSTEM_SESSION_PROCESS_INFORMATION) + 16];
    PSYSTEM_SESSION_PROCESS_INFORMATION Info;
    NTSTATUS Status;
    ULONG i, Ret;

    Info = (PSYSTEM_SESSION_PROCESS_INFORMATION)(((ULONG_PTR)Raw + 7) & ~(ULONG_PTR)7);
    for (i = 0; i < _countof(Lengths); i++)
    {
        Info->SessionId = OwnSession;
        Info->SizeOfBuf = BIG_SIZE;
        Info->Buffer = Buffer;
        Ret = 0xDEADBEEF;
        Status = NtQuerySystemInformation(SystemSessionProcessInformation, Info, Lengths[i], &Ret);
        trace("PROBE P2 outer length %lu -> 0x%lx ret=%lu\n", Lengths[i], Status, Ret);
    }

    Info = (PSYSTEM_SESSION_PROCESS_INFORMATION)((PUCHAR)Info + 1);
    Info->SessionId = OwnSession;
    Info->SizeOfBuf = BIG_SIZE;
    Info->Buffer = Buffer;
    Ret = 0xDEADBEEF;
    Status = NtQuerySystemInformation(SystemSessionProcessInformation, Info, sizeof(*Info), &Ret);
    trace("PROBE P3 outer misaligned +1 -> 0x%lx ret=%lu\n", Status, Ret);
}

/* P4 + P5 + P6 + P7: the inner buffer */
static
VOID
TestInner(PUCHAR Buffer)
{
    static const ULONG Sizes[] = { 16, sizeof(SYSTEM_PROCESS_INFORMATION) - 1, sizeof(SYSTEM_PROCESS_INFORMATION) };
    static const ULONG Misalign[] = { 1, 4 };
    PVOID Kernel = (PVOID)((ULONG_PTR)1 << (sizeof(PVOID) * 8 - 1));
    NTSTATUS Status;
    ULONG i, Ret;

    Ret = 0xDEADBEEF;
    Status = QuerySession(OwnSession, NULL, 0, &Ret);
    trace("PROBE P4 inner NULL/0 -> 0x%lx ret=%lu\n", Status, Ret);

    for (i = 0; i < _countof(Sizes); i++)
    {
        Ret = 0xDEADBEEF;
        Status = QuerySession(OwnSession, Buffer, Sizes[i], &Ret);
        trace("PROBE P5 inner size %lu -> 0x%lx ret=%lu\n", Sizes[i], Status, Ret);
    }

    for (i = 0; i < _countof(Misalign); i++)
    {
        Ret = 0xDEADBEEF;
        Status = QuerySession(OwnSession, Buffer + Misalign[i], BIG_SIZE - 8, &Ret);
        trace("PROBE P6 inner misaligned +%lu -> 0x%lx ret=%lu\n", Misalign[i], Status, Ret);
    }

    Ret = 0xDEADBEEF;
    Status = QuerySession(OwnSession, (PVOID)(ULONG_PTR)0x1000, BIG_SIZE, &Ret);
    trace("PROBE P7 inner 0x1000 -> 0x%lx ret=%lu\n", Status, Ret);
    Ret = 0xDEADBEEF;
    Status = QuerySession(OwnSession, Kernel, BIG_SIZE, &Ret);
    trace("PROBE P7 inner kernel %p -> 0x%lx ret=%lu\n", Kernel, Status, Ret);
}

/* P8: other session ids, with the caller's TCB state, then with TCB disabled */
static
VOID
TestOtherSessions(PUCHAR Buffer)
{
    ULONG Sessions[3], i, Ret, Count, Foreign;
    BOOLEAN Found, Old, Pass;
    NTSTATUS Status;

    Sessions[0] = OwnSession ? 0 : 1;
    Sessions[1] = OwnSession + 1;
    Sessions[2] = 0xFFFFFFFF;
    for (Pass = 0; Pass < 2; Pass++)
    {
        if (Pass == 1)
        {
            if (!HaveTcb)
                break;
            RtlAdjustPrivilege(SE_TCB_PRIVILEGE, FALSE, FALSE, &Old);
        }
        for (i = 0; i < _countof(Sessions); i++)
        {
            Ret = 0xDEADBEEF;
            Status = QuerySession(Sessions[i], Buffer, BIG_SIZE, &Ret);
            Count = Foreign = 0;
            Found = FALSE;
            if (NT_SUCCESS(Status))
                Count = WalkChain(Buffer, BIG_SIZE, Sessions[i], &Foreign, NULL, &Found);
            trace("PROBE P8 tcb=%s session %lu -> 0x%lx ret=%lu records=%lu foreign=%lu\n",
                  (HaveTcb && Pass == 0) ? "on" : "off", Sessions[i], Status, Ret, Count, Foreign);
            if (NT_SUCCESS(Status))
                ok(Foreign == 0, "session %lu: %lu records from another session\n", Sessions[i], Foreign);
        }
    }
    if (HaveTcb)
        RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old);
}

START_TEST(SystemSessionProcessInformation)
{
    PUCHAR Buffer;
    BOOLEAN Old;

    OwnSession = NtCurrentPeb()->SessionId;
    HaveTcb = NT_SUCCESS(RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &Old));
    trace("session %lu, TCB %s\n", OwnSession, HaveTcb ? "held" : "not held");

    Buffer = VirtualAlloc(NULL, BIG_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!Buffer)
    {
        skip("no buffer\n");
        return;
    }
    TestOwnSession(Buffer);
    TestOuter(Buffer);
    TestInner(Buffer);
    TestOtherSessions(Buffer);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}
