/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Power Manager: processor idle accounting, processor wake, coalescing callbacks
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#pragma once

/* Coalescing callbacks (wdm.h, Windows 8 and later) */
typedef
_Function_class_(PO_COALESCING_CALLBACK)
_IRQL_requires_same_
VOID
NTAPI
PO_COALESCING_CALLBACK(
    _In_ BOOLEAN Enabled,
    _In_opt_ PVOID Context);
typedef PO_COALESCING_CALLBACK *PPO_COALESCING_CALLBACK;

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
PoRegisterCoalescingCallback(
    _In_ PPO_COALESCING_CALLBACK Callback,
    _In_ BOOLEAN ImmediateCallback,
    _Outptr_ PVOID *RegistrationHandle,
    _In_opt_ PVOID Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PoUnregisterCoalescingCallback(
    _In_ PVOID RegistrationHandle);

/* Internal: the policy side reports when coalescing (idle resiliency) turns on or off */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PopSetCoalescingState(
    _In_ BOOLEAN Enabled);

BOOLEAN
NTAPI
PopGetCoalescingState(VOID);

/*
 * Processor idle accounting. There is no public layout for this data, so the
 * structure below is LiberNT's own, versioned so it can grow. All times are
 * in 100 ns units.
 */
#define PO_IDLE_ACCOUNTING_VERSION 1
#define PO_IDLE_ACCOUNTING_MAX_STATES 3

typedef struct _PO_IDLE_STATE_ACCOUNTING
{
    ULONGLONG TotalTime;
    ULONG IdleTransitions;
    ULONG Reserved;
} PO_IDLE_STATE_ACCOUNTING, *PPO_IDLE_STATE_ACCOUNTING;

typedef struct _PO_PROCESSOR_IDLE_ACCOUNTING
{
    ULONG Version;
    ULONG ProcessorIndex;
    ULONG StateCount;           /* registered idle states (0: only the default halt loop) */
    ULONG CurrentState;         /* idle state the processor will use next */
    ULONGLONG IdleTime;         /* time the idle thread ran */
    ULONGLONG KernelTime;       /* kernel time, idle thread included */
    ULONGLONG UserTime;
    ULONGLONG InterruptTime;    /* KeQueryInterruptTime at sampling */
    ULONG PromotionCount;
    ULONG DemotionCount;
    ULONG ErrorCount;
    ULONG Reserved;
    PO_IDLE_STATE_ACCOUNTING State[PO_IDLE_ACCOUNTING_MAX_STATES];
} PO_PROCESSOR_IDLE_ACCOUNTING, *PPO_PROCESSOR_IDLE_ACCOUNTING;

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PoGetProcessorIdleAccounting(
    _In_ ULONG ProcessorIndex,
    _Out_writes_bytes_to_(BufferLength, *ReturnLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_opt_ PULONG ReturnLength);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PoInitiateProcessorWake(
    _In_ ULONG ProcessorIndex);

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
PoCpuIdledSinceLastCallImprecise(
    _In_ ULONG ProcessorIndex);
