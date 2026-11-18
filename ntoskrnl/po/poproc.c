/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Power Manager: processor idle accounting, processor wake, coalescing callbacks
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>
#include <internal/po_proc.h>
#define NDEBUG
#include <debug.h>

#define TAG_PO_COALESCING 'cPoP'
#define POP_COALESCING_SIGNATURE 'lCoP'

/* PROCESSOR IDLE ACCOUNTING *************************************************/

C_ASSERT(PO_IDLE_ACCOUNTING_MAX_STATES <=
         RTL_NUMBER_OF_FIELD(PROCESSOR_POWER_STATE, TotalIdleStateTime));

static
PKPRCB
PopGetPrcbFromIndex(
    _In_ ULONG ProcessorIndex)
{
    if (ProcessorIndex >= (ULONG)KeNumberProcessors ||
        ProcessorIndex >= MAXIMUM_PROCESSORS)
    {
        return NULL;
    }
    return KiProcessorBlock[ProcessorIndex];
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PopGetProcessorIdleAccounting(
    _In_ ULONG ProcessorIndex,
    _Out_writes_bytes_to_(BufferLength, *ReturnLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PULONG ReturnLength)
{
    PPO_PROCESSOR_IDLE_ACCOUNTING Accounting = Buffer;
    PPROCESSOR_POWER_STATE PowerState;
    PKTHREAD IdleThread;
    PKPRCB Prcb;
    ULONG StateCount;
    ULONG Index;

    if (ReturnLength)
        *ReturnLength = sizeof(PO_PROCESSOR_IDLE_ACCOUNTING);

    Prcb = PopGetPrcbFromIndex(ProcessorIndex);
    if (!Prcb)
        return STATUS_INVALID_PARAMETER_1;
    if (!Buffer)
        return STATUS_INVALID_PARAMETER_2;
    if (BufferLength < sizeof(PO_PROCESSOR_IDLE_ACCOUNTING))
        return STATUS_BUFFER_TOO_SMALL;

    /*
     * The counters belong to another processor and are updated without a lock.
     * Each field is read once; the snapshot is consistent per field, not as a whole.
     */
    PowerState = &Prcb->PowerState;
    RtlZeroMemory(Accounting, sizeof(*Accounting));
    Accounting->Version = PO_IDLE_ACCOUNTING_VERSION;
    Accounting->ProcessorIndex = ProcessorIndex;

    StateCount = *(volatile ULONG *)&PowerState->IdleHandlersCount;
    Accounting->StateCount = StateCount;
    Accounting->CurrentState = (ULONG)(ULONG_PTR)*(PVOID volatile *)&PowerState->IdleState;

    IdleThread = Prcb->IdleThread;
    if (IdleThread)
        Accounting->IdleTime = (ULONGLONG)*(volatile ULONG *)&IdleThread->KernelTime * KeMaximumIncrement;
    Accounting->KernelTime = (ULONGLONG)*(volatile ULONG *)&Prcb->KernelTime * KeMaximumIncrement;
    Accounting->UserTime = (ULONGLONG)*(volatile ULONG *)&Prcb->UserTime * KeMaximumIncrement;
    Accounting->InterruptTime = KeQueryInterruptTime();
    Accounting->PromotionCount = PowerState->PromotionCount;
    Accounting->DemotionCount = PowerState->DemotionCount;
    Accounting->ErrorCount = PowerState->ErrorCount;

    /* Per-state residency is only kept by PopIdleRegistered (registered idle handlers) */
    for (Index = 0; Index < PO_IDLE_ACCOUNTING_MAX_STATES && Index < StateCount; Index++)
    {
        Accounting->State[Index].TotalTime = PowerState->TotalIdleStateTime[Index];
        Accounting->State[Index].IdleTransitions = PowerState->TotalIdleTransitions[Index];
    }

    return STATUS_SUCCESS;
}

/* Last idle-thread tick count seen by PopCpuIdledSinceLastCallImprecise, per processor */
static volatile LONG PopIdleTickSnapshot[MAXIMUM_PROCESSORS];

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
PopCpuIdledSinceLastCallImprecise(
    _In_ ULONG ProcessorIndex)
{
    PKTHREAD IdleThread;
    PKPRCB Prcb;
    LONG Ticks;
    LONG Previous;

    Prcb = PopGetPrcbFromIndex(ProcessorIndex);
    if (!Prcb)
        return FALSE;
    IdleThread = Prcb->IdleThread;
    if (!IdleThread)
        return FALSE;

    /*
     * Imprecise by design: idle time is charged a whole clock tick at a time,
     * so idling shorter than a tick may go unseen until a tick lands in it.
     */
    Ticks = (LONG)*(volatile ULONG *)&IdleThread->KernelTime;
    Previous = InterlockedExchange(&PopIdleTickSnapshot[ProcessorIndex], Ticks);
    return (Ticks != Previous);
}

/* PROCESSOR WAKE ************************************************************/

/* 0: not initialized, 1: initializing, 2: ready */
static volatile LONG PopWakeDpcState;
static KDPC PopWakeDpc[MAXIMUM_PROCESSORS];

static
VOID
NTAPI
PopProcessorWakeDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /*
     * Nothing to do: the interrupt that delivered this DPC already took the
     * processor out of its idle state, and the idle loop re-evaluates its
     * ready queue on the way back.
     */
}

static
VOID
PopInitializeWakeDpcs(VOID)
{
    ULONG Index;

    if (PopWakeDpcState == 2)
        return;

    if (InterlockedCompareExchange(&PopWakeDpcState, 1, 0) == 0)
    {
        for (Index = 0; Index < MAXIMUM_PROCESSORS; Index++)
        {
            KeInitializeDpc(&PopWakeDpc[Index], PopProcessorWakeDpc, NULL);
            KeSetTargetProcessorDpc(&PopWakeDpc[Index], (CCHAR)Index);
            KeSetImportanceDpc(&PopWakeDpc[Index], HighImportance);
        }
        InterlockedExchange(&PopWakeDpcState, 2);
        return;
    }

    while (PopWakeDpcState != 2)
        YieldProcessor();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PopInitiateProcessorWake(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB Prcb;

    Prcb = PopGetPrcbFromIndex(ProcessorIndex);
    if (!Prcb)
        return STATUS_INVALID_PARAMETER;

    /* The caller's own processor is running by definition */
    if (Prcb == KeGetCurrentPrcb())
        return STATUS_SUCCESS;

    PopInitializeWakeDpcs();

    /*
     * A high-importance DPC targeted at a remote processor is delivered with
     * an IPI, which brings that processor out of HLT/WFI. If the DPC is still
     * queued, a wake is already on its way and nothing more is needed.
     */
    KeInsertQueueDpc(&PopWakeDpc[ProcessorIndex], NULL, NULL);
    return STATUS_SUCCESS;
}

/* COALESCING CALLBACKS ******************************************************/

typedef struct _POP_COALESCING_REGISTRATION
{
    ULONG Signature;
    LIST_ENTRY Link;
    PPO_COALESCING_CALLBACK Callback;
    PVOID Context;
} POP_COALESCING_REGISTRATION, *PPOP_COALESCING_REGISTRATION;

/* Zero-initialized push lock and list head set up on first use: no PoInit hook needed */
static EX_PUSH_LOCK PopCoalescingLock;
static LIST_ENTRY PopCoalescingList = { &PopCoalescingList, &PopCoalescingList };
static volatile LONG PopCoalescingEnabled;

BOOLEAN
NTAPI
PopGetCoalescingState(VOID)
{
    return (PopCoalescingEnabled != 0);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PopSetCoalescingState(
    _In_ BOOLEAN Enabled)
{
    PPOP_COALESCING_REGISTRATION Registration;
    PLIST_ENTRY Entry;
    LONG NewValue = Enabled ? 1 : 0;

    PAGED_CODE();

    /*
     * The state change and the notification walk are serialized by the lock
     * against other changes and against Unregister, so registrants see the
     * changes in order and, once Unregister returns, the callback is never
     * entered again. Callbacks run at PASSIVE_LEVEL and must
     * not register or unregister from inside the callback.
     */
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&PopCoalescingLock);

    if (InterlockedExchange(&PopCoalescingEnabled, NewValue) != NewValue)
    {
        for (Entry = PopCoalescingList.Flink; Entry != &PopCoalescingList; Entry = Entry->Flink)
        {
            Registration = CONTAINING_RECORD(Entry, POP_COALESCING_REGISTRATION, Link);
            Registration->Callback(Enabled, Registration->Context);
        }
    }

    ExReleasePushLockExclusive(&PopCoalescingLock);
    KeLeaveCriticalRegion();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
PoRegisterCoalescingCallback(
    _In_ PPO_COALESCING_CALLBACK Callback,
    _In_ BOOLEAN ImmediateCallback,
    _Outptr_ PVOID *RegistrationHandle,
    _In_opt_ PVOID Context)
{
    PPOP_COALESCING_REGISTRATION Registration;
    BOOLEAN Enabled;

    PAGED_CODE();

    if (!RegistrationHandle)
        return STATUS_INVALID_PARAMETER;
    *RegistrationHandle = NULL;
    if (!Callback)
        return STATUS_INVALID_PARAMETER;

    Registration = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Registration), TAG_PO_COALESCING);
    if (!Registration)
        return STATUS_INSUFFICIENT_RESOURCES;
    Registration->Signature = POP_COALESCING_SIGNATURE;
    Registration->Callback = Callback;
    Registration->Context = Context;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&PopCoalescingLock);
    InsertTailList(&PopCoalescingList, &Registration->Link);
    if (ImmediateCallback)
    {
        /* Still under the lock: no state change can be delivered before this report */
        Enabled = PopGetCoalescingState();
        Callback(Enabled, Context);
    }
    ExReleasePushLockExclusive(&PopCoalescingLock);
    KeLeaveCriticalRegion();

    *RegistrationHandle = Registration;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PoUnregisterCoalescingCallback(
    _In_ PVOID RegistrationHandle)
{
    PPOP_COALESCING_REGISTRATION Registration = RegistrationHandle;

    PAGED_CODE();

    if (!Registration)
        return;
    if (Registration->Signature != POP_COALESCING_SIGNATURE)
    {
        DPRINT1("PoUnregisterCoalescingCallback: bad handle %p\n", RegistrationHandle);
        ASSERT(FALSE);
        return;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&PopCoalescingLock);
    RemoveEntryList(&Registration->Link);
    ExReleasePushLockExclusive(&PopCoalescingLock);
    KeLeaveCriticalRegion();

    Registration->Signature = 0;
    ExFreePoolWithTag(Registration, TAG_PO_COALESCING);
}
