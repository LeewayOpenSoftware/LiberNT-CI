/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Second batch of ntoskrnl exports that replaced spec stubs
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

static volatile LONG g_LegacyThreadCalls;
static volatile LONG g_NonSystemThreadCalls;
static PVOID g_WatchedThreadId;

static VOID
NTAPI
LegacyThreadNotify(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create)
{
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(Create);
    if (ThreadId == g_WatchedThreadId)
        InterlockedIncrement(&g_LegacyThreadCalls);
}

static VOID
NTAPI
NonSystemThreadNotify(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create)
{
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(Create);
    if (ThreadId == g_WatchedThreadId)
        InterlockedIncrement(&g_NonSystemThreadCalls);
}

static KEVENT g_ThreadGo;

static VOID
NTAPI
IdleSystemThread(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeWaitForSingleObject(&g_ThreadGo, Executive, KernelMode, FALSE, NULL);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS
NTAPI
LogonTerminated(
    _In_ PLUID LogonId,
    _In_ PESILO ServerSilo,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(LogonId);
    UNREFERENCED_PARAMETER(ServerSilo);
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

static VOID
TestBitmaps(VOID)
{
    RTL_BITMAP Bits, Out;
    ULONG Buffer[2], OutBuffer[2];

    RtlInitializeBitMap(&Bits, Buffer, 64);
    RtlClearAllBits(&Bits);
    RtlSetBits(&Bits, 4, 8);     /* bits 4..11 */
    RtlSetBit(&Bits, 40);

    ok_eq_ulong(RtlNumberOfClearBitsInRange(&Bits, 0, 64), 55UL);
    ok_eq_ulong(RtlNumberOfClearBitsInRange(&Bits, 4, 8), 0UL);
    ok_eq_ulong(RtlNumberOfClearBitsInRange(&Bits, 0, 16), 8UL);
    ok_eq_ulong(RtlNumberOfClearBitsInRange(&Bits, 64, 1), MAXULONG);

    RtlInitializeBitMap(&Out, OutBuffer, 64);
    RtlSetAllBits(&Out);
    RtlExtractBitMap(&Bits, &Out, 2, 12);
    ok_eq_ulong(OutBuffer[0] & 0xFFF, 0x3FCUL);  /* source bits 2..13 */
    ok_eq_ulong(OutBuffer[0] >> 12, 0xFFFFFUL);  /* bits past NumberOfBits are untouched */

    RtlClearAllBits(&Out);
    RtlExtractBitMap(&Bits, &Out, 40, 100);      /* clamped to the end of the source */
    ok(RtlTestBit(&Out, 0), "source bit 40 lands on bit 0\n");
    ok_eq_ulong(RtlNumberOfSetBits(&Out), 1UL);

    /* In place, as Destination may alias Source */
    RtlExtractBitMap(&Bits, &Bits, 4, 8);
    ok_eq_ulong(Buffer[0] & 0xFF, 0xFFUL);
}

static VOID
TestInt64(VOID)
{
    UNICODE_STRING String;
    LONG64 Value;
    PWSTR End;
    NTSTATUS Status;

    RtlInitUnicodeString(&String, L"  -9223372036854775807xyz");
    Status = RtlUnicodeStringToInt64(&String, 0, &Value, &End);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Value == -9223372036854775807LL, "value %I64d\n", Value);
    ok(End == String.Buffer + 22, "end at %Iu\n", (ULONG_PTR)(End - String.Buffer));

    RtlInitUnicodeString(&String, L"0x1FFFFFFFF");
    Status = RtlUnicodeStringToInt64(&String, 0, &Value, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Value == 0x1FFFFFFFFLL, "value %I64x\n", Value);

    RtlInitUnicodeString(&String, L"777");
    Status = RtlUnicodeStringToInt64(&String, 8, &Value, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Value == 511, "value %I64d\n", Value);

    Status = RtlUnicodeStringToInt64(&String, 7, &Value, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

static VOID
TestPushList(VOID)
{
    DECLSPEC_ALIGN(16) SLIST_HEADER Head;
    DECLSPEC_ALIGN(16) SLIST_ENTRY Entries[3];
    PSLIST_ENTRY Previous;

    InitializeSListHead(&Head);
    Entries[0].Next = &Entries[1];
    Entries[1].Next = &Entries[2];
    Entries[2].Next = NULL;

    Previous = InterlockedPushListSList(&Head, &Entries[0], &Entries[2], 3);
    ok(Previous == NULL, "empty list returned %p\n", Previous);
    ok_eq_uint(ExQueryDepthSList(&Head), 3);
    ok(InterlockedPopEntrySList(&Head) == &Entries[0], "chain head pops first\n");
    ok(InterlockedPopEntrySList(&Head) == &Entries[1], "chain order kept\n");
    ok(InterlockedPopEntrySList(&Head) == &Entries[2], "chain end pops last\n");
    ok(InterlockedPopEntrySList(&Head) == NULL, "list empty again\n");
}

static VOID
TestSilos(VOID)
{
    OBJECT_ATTRIBUTES Attributes;
    HANDLE JobHandle;
    PEJOB Job;
    PESILO Silo = (PESILO)(ULONG_PTR)1;
    GUID *ContainerId;
    static const GUID NullGuid;
    NTSTATUS Status;

    ok_eq_hex(PsGetJobSilo(NULL, &Silo), STATUS_INVALID_PARAMETER);
    ok_eq_hex(PsGetJobServerSilo(NULL, &Silo), STATUS_INVALID_PARAMETER);

    ContainerId = PsGetSiloContainerId(PsGetHostSilo());
    ok(ContainerId != NULL && RtlEqualMemory(ContainerId, &NullGuid, sizeof(GUID)),
       "host silo container id is the null GUID\n");

    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateJobObject(&JobHandle, JOB_OBJECT_ALL_ACCESS, &Attributes);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = ObReferenceObjectByHandle(JobHandle, JOB_OBJECT_ALL_ACCESS, PsJobType, KernelMode, (PVOID *)&Job, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_hex(PsGetJobSilo(Job, &Silo), STATUS_JOB_NO_CONTAINER);
        ok(Silo == NULL, "no silo for a plain job, got %p\n", Silo);

        Silo = (PESILO)(ULONG_PTR)1;
        ok_eq_hex(PsGetJobServerSilo(Job, &Silo), STATUS_SUCCESS);
        ok(PsIsHostSilo(Silo), "a plain job runs in the host silo, got %p\n", Silo);
        ObDereferenceObject(Job);
    }

    ZwClose(JobHandle);
}

static VOID
TestThreads(VOID)
{
    LARGE_INTEGER Now;
    LONGLONG Created;
    HANDLE ThreadHandle;
    PETHREAD Thread;
    NTSTATUS Status;

    Created = PsGetThreadCreateTime(PsGetCurrentThread());
    KeQuerySystemTime(&Now);
    ok(Created > 0 && Created <= Now.QuadPart, "create time %I64d, now %I64d\n", Created, Now.QuadPart);

    ok_eq_hex(PsSetCreateThreadNotifyRoutineEx(PsCreateThreadNotifyNonSystem, NULL), STATUS_INVALID_PARAMETER);
    ok_eq_hex(PsSetCreateThreadNotifyRoutineEx((PSCREATETHREADNOTIFYTYPE)7, (PVOID)NonSystemThreadNotify),
              STATUS_INVALID_PARAMETER);

    Status = PsSetCreateThreadNotifyRoutineEx(PsCreateThreadNotifyNonSystem, (PVOID)NonSystemThreadNotify);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = PsSetCreateThreadNotifyRoutineEx(PsCreateThreadNotifySubsystems, (PVOID)LegacyThreadNotify);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* A system thread reaches the subsystem routine but never the non-system one */
    g_WatchedThreadId = (PVOID)(ULONG_PTR)-4;
    g_LegacyThreadCalls = 0;
    g_NonSystemThreadCalls = 0;
    KeInitializeEvent(&g_ThreadGo, NotificationEvent, FALSE);
    Status = PsCreateSystemThread(&ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, IdleSystemThread, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(ThreadHandle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&Thread, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            g_WatchedThreadId = PsGetThreadId(Thread);
            KeSetEvent(&g_ThreadGo, IO_NO_INCREMENT, FALSE);
            KeWaitForSingleObject(Thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(Thread);
            ok_eq_long(g_LegacyThreadCalls, 1L);   /* the exit; creation ran before the id was known */
            ok_eq_long(g_NonSystemThreadCalls, 0L);
        }
        ZwClose(ThreadHandle);
    }

    ok_eq_hex(PsRemoveCreateThreadNotifyRoutine(LegacyThreadNotify), STATUS_SUCCESS);
    ok_eq_hex(PsRemoveCreateThreadNotifyRoutine(NonSystemThreadNotify), STATUS_SUCCESS);
    ok_eq_hex(PsRemoveCreateThreadNotifyRoutine(NonSystemThreadNotify), STATUS_PROCEDURE_NOT_FOUND);
}

static VOID
TestLogonRegistration(VOID)
{
    int Context1, Context2;

    ok_eq_hex(SeRegisterLogonSessionTerminatedRoutineEx(NULL, &Context1), STATUS_INVALID_PARAMETER);
    ok_eq_hex(SeRegisterLogonSessionTerminatedRoutineEx(LogonTerminated, &Context1), STATUS_SUCCESS);
    ok_eq_hex(SeUnregisterLogonSessionTerminatedRoutineEx(LogonTerminated, &Context2), STATUS_NOT_FOUND);
    ok_eq_hex(SeUnregisterLogonSessionTerminatedRoutineEx(LogonTerminated, &Context1), STATUS_SUCCESS);
    ok_eq_hex(SeUnregisterLogonSessionTerminatedRoutineEx(LogonTerminated, &Context1), STATUS_NOT_FOUND);
}

static VOID
TestMemory(VOID)
{
    SECTION_OBJECT_POINTERS Pointers;
    PHYSICAL_ADDRESS Low, High, Skip;
    PMDL Mdl;
    PUCHAR Va;

    RtlZeroMemory(&Pointers, sizeof(Pointers));
    ok(MmForceSectionClosedEx(&Pointers, MM_FORCE_CLOSED_DATA), "no data section to close\n");
    ok(MmForceSectionClosedEx(&Pointers, MM_FORCE_CLOSED_IMAGE | MM_FORCE_CLOSED_LATER_OK), "no image section\n");
    ok(MmForceSectionClosedEx(&Pointers, MM_FORCE_CLOSED_DATA | MM_FORCE_CLOSED_IMAGE), "no section at all\n");

    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;
    Mdl = MmAllocatePagesForMdlEx(Low, High, Skip, 2 * PAGE_SIZE, MmCached, 0);
    ok(Mdl != NULL, "page allocation failed\n");
    if (Mdl == NULL) return;

    Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    ok(Va != NULL, "mapping failed\n");
    if (Va != NULL)
    {
        RtlFillMemory(Va, MmGetMdlByteCount(Mdl), 0xA5);
        MmUnmapLockedPages(Va, Mdl);
    }
    MmFreePagesFromMdlEx(Mdl, MM_FREE_MDL_PAGES_ZERO);
    ExFreePool(Mdl);
}

static VOID
TestBaseMcb(VOID)
{
    BASE_MCB Mcb;
    LONGLONG Lbn, Count;

    ok(FsRtlInitializeBaseMcbEx(&Mcb, PagedPool, 0), "init failed\n");
    ok_eq_uint(Mcb.Flags, 0);
    ok(FsRtlAddBaseMcbEntry(&Mcb, 0, 100, 8), "add failed\n");
    ok(FsRtlLookupBaseMcbEntry(&Mcb, 4, &Lbn, &Count, NULL, NULL, NULL), "lookup failed\n");
    ok(Lbn == 104 && Count == 4, "lbn %I64d count %I64d\n", Lbn, Count);
    FsRtlUninitializeBaseMcb(&Mcb);

    ok(FsRtlInitializeBaseMcbEx(&Mcb, PagedPool, MCB_FLAG_RAISE_ON_ALLOCATION_FAILURE), "init failed\n");
    ok_eq_uint(Mcb.Flags, MCB_FLAG_RAISE_ON_ALLOCATION_FAILURE);
    FsRtlUninitializeBaseMcb(&Mcb);
}

START_TEST(NtosStubExports2)
{
    TestBitmaps();
    TestInt64();
    TestPushList();
    TestSilos();
    TestThreads();
    TestLogonRegistration();
    TestMemory();
    TestBaseMcb();
}
