/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Processor idle accounting, processor wake and coalescing callback exports
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

typedef VOID (NTAPI *PTEST_COALESCING_CALLBACK)(_In_ BOOLEAN Enabled, _In_opt_ PVOID Context);

typedef struct _TEST_IDLE_STATE_ACCOUNTING
{
    ULONGLONG TotalTime;
    ULONG IdleTransitions;
    ULONG Reserved;
} TEST_IDLE_STATE_ACCOUNTING;

/* Mirrors PO_PROCESSOR_IDLE_ACCOUNTING version 1 (ntoskrnl/include/internal/po_proc.h) */
typedef struct _TEST_IDLE_ACCOUNTING
{
    ULONG Version;
    ULONG ProcessorIndex;
    ULONG StateCount;
    ULONG CurrentState;
    ULONGLONG IdleTime;
    ULONGLONG KernelTime;
    ULONGLONG UserTime;
    ULONGLONG InterruptTime;
    ULONG PromotionCount;
    ULONG DemotionCount;
    ULONG ErrorCount;
    ULONG Reserved;
    TEST_IDLE_STATE_ACCOUNTING State[3];
} TEST_IDLE_ACCOUNTING;

/* Internal: no matching public prototype in WDK 26100; define to test an export. */
#ifdef KMT_PO_INTERNAL_EXPORTS
NTSYSAPI NTSTATUS NTAPI PoGetProcessorIdleAccounting(_In_ ULONG ProcessorIndex, _Out_ PVOID Buffer, _In_ ULONG BufferLength, _Out_opt_ PULONG ReturnLength);
NTSYSAPI NTSTATUS NTAPI PoInitiateProcessorWake(_In_ ULONG ProcessorIndex);
NTSYSAPI BOOLEAN NTAPI PoCpuIdledSinceLastCallImprecise(_In_ ULONG ProcessorIndex);
#endif
NTSYSAPI NTSTATUS NTAPI PoRegisterCoalescingCallback(_In_ PTEST_COALESCING_CALLBACK Callback, _In_ BOOLEAN ImmediateCallback, _Outptr_ PVOID *RegistrationHandle, _In_opt_ PVOID Context);
NTSYSAPI VOID NTAPI PoUnregisterCoalescingCallback(_In_ PVOID RegistrationHandle);

#ifdef KMT_PO_INTERNAL_EXPORTS
static
VOID
TestIdleAccounting(VOID)
{
    TEST_IDLE_ACCOUNTING First, Second;
    ULONG Count = (ULONG)KeNumberProcessors;
    ULONG Length;
    ULONG Index;
    NTSTATUS Status;

    Length = 0;
    Status = PoGetProcessorIdleAccounting(Count, &First, sizeof(First), &Length);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER_1);
    Status = PoGetProcessorIdleAccounting(0, NULL, sizeof(First), NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER_2);
    Length = 0;
    Status = PoGetProcessorIdleAccounting(0, &First, sizeof(First) - 1, &Length);
    ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);
    ok_eq_ulong(Length, (ULONG)sizeof(First));

    for (Index = 0; Index < Count; Index++)
    {
        RtlFillMemory(&First, sizeof(First), 0xA5);
        Status = PoGetProcessorIdleAccounting(Index, &First, sizeof(First), &Length);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(First.Version, 1UL);
        ok_eq_ulong(First.ProcessorIndex, Index);
        ok(First.StateCount <= 32, "CPU %lu: %lu idle states\n", Index, First.StateCount);
        ok(First.InterruptTime != 0, "CPU %lu: no interrupt time\n", Index);

        Status = PoGetProcessorIdleAccounting(Index, &Second, sizeof(Second), NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Second.IdleTime >= First.IdleTime, "CPU %lu: idle time went back\n", Index);
        ok(Second.KernelTime >= First.KernelTime, "CPU %lu: kernel time went back\n", Index);
        ok(Second.InterruptTime >= First.InterruptTime, "CPU %lu: interrupt time went back\n", Index);
    }
}

static
VOID
TestProcessorWake(VOID)
{
    ULONG Count = (ULONG)KeNumberProcessors;
    ULONG Index;
    LARGE_INTEGER Delay;

    ok_eq_hex(PoInitiateProcessorWake(Count), STATUS_INVALID_PARAMETER);
    ok_eq_hex(PoInitiateProcessorWake(KeGetCurrentProcessorNumber()), STATUS_SUCCESS);
    for (Index = 0; Index < Count; Index++)
    {
        ok_eq_hex(PoInitiateProcessorWake(Index), STATUS_SUCCESS);
        /* A second request while the first may still be queued is folded into it */
        ok_eq_hex(PoInitiateProcessorWake(Index), STATUS_SUCCESS);
    }

    ok_bool_false(PoCpuIdledSinceLastCallImprecise(Count), "Out-of-range CPU");

    /* Prime processor 0, then give it time to run its idle thread for at least a tick */
    (VOID)PoCpuIdledSinceLastCallImprecise(0);
    Delay.QuadPart = -200 * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    /* Assumes the test machine is mostly idle; imprecise by contract */
    ok_bool_true(PoCpuIdledSinceLastCallImprecise(0), "CPU 0 idled during a 200 ms sleep");
}

#endif

typedef struct _TEST_CONTEXT
{
    LONG Calls;
    BOOLEAN LastEnabled;
} TEST_CONTEXT, *PTEST_CONTEXT;

static
VOID
NTAPI
TestCoalescingCallback(
    _In_ BOOLEAN Enabled,
    _In_opt_ PVOID Context)
{
    PTEST_CONTEXT TestContext = Context;

    ok_irql(PASSIVE_LEVEL);
    if (!TestContext)
        return;
    InterlockedIncrement(&TestContext->Calls);
    TestContext->LastEnabled = Enabled;
}

static
VOID
TestCoalescingCallbacks(VOID)
{
    TEST_CONTEXT First = { 0 }, Second = { 0 };
    PVOID Handle1 = (PVOID)1, Handle2 = NULL;
    NTSTATUS Status;

    Status = PoRegisterCoalescingCallback(NULL, TRUE, &Handle1, &First);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok(Handle1 == NULL, "A failed registration returned the handle %p\n", Handle1);
    ok_eq_long(First.Calls, 0L);

    Status = PoRegisterCoalescingCallback(TestCoalescingCallback, TRUE, &Handle1, &First);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle1 != NULL, "No handle\n");
    ok_eq_long(First.Calls, 1L);

    Status = PoRegisterCoalescingCallback(TestCoalescingCallback, FALSE, &Handle2, &Second);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle2 != NULL && Handle2 != Handle1, "Handles %p and %p\n", Handle1, Handle2);
    ok_eq_long(Second.Calls, 0L);

    if (Handle2)
        PoUnregisterCoalescingCallback(Handle2);
    if (Handle1)
        PoUnregisterCoalescingCallback(Handle1);
    PoUnregisterCoalescingCallback(NULL);
    ok_eq_long(First.Calls, 1L);
    ok_eq_long(Second.Calls, 0L);
}

START_TEST(E2_PoProc)
{
#ifdef KMT_PO_INTERNAL_EXPORTS
    TestIdleAccounting();
    TestProcessorWake();
#else
    skip(FALSE, "Processor idle routines are internal\n");
#endif
    TestCoalescingCallbacks();
}
