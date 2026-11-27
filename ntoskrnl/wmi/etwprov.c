/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Event tracing providers: registration, enable state, event delivery
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * One provider entry per GUID, created by the first registration or the first
 * enable (a session may enable a provider before it registers). Each entry
 * holds the sessions it is enabled for, with their level and keywords, and an
 * aggregate the writers read without a lock. Provider entries are never freed,
 * so a lock-free reader may always dereference one.
 *
 * Kernel registrations live in a slot table: a REGHANDLE is the slot number
 * with the slot's generation above it, so a stale handle is refused. Enable
 * callbacks run at PASSIVE_LEVEL, outside every lock, under a rundown
 * reference that EtwUnregister waits for.
 *
 * Events are delivered at IRQL <= APC_LEVEL (the session buffers are guarded by
 * a guarded mutex); above that an event is dropped. User-mode providers write
 * through NtTraceEvent with their registration handle. User-mode enable
 * callbacks are not delivered yet (no notification channel to the process).
 */

#include <ntoskrnl.h>
#include <ntwmi.h>
#include "wmip.h"

#define NDEBUG
#include <debug.h>

#define ETWP_PROV_TAG           'PwtE'
#define ETWP_FILTERS_TAG        'tFtE'
#define ETWP_MAX_SESSIONS       8       /* sessions one provider can be enabled for */
#define ETWP_SLOTS_PER_BLOCK    256
#define ETWP_MAX_BLOCKS         256
#define ETWP_LEVEL_ALL          0xFF

/* Win11 rejects filter types a session does not support; unknown types
   are an error here as well */
static
BOOLEAN
EtwpFilterTypeKnown(IN ULONG Type)
{
    switch (Type)
    {
    case EVENT_FILTER_TYPE_NONE:
    case EVENT_FILTER_TYPE_SCHEMATIZED:
    case EVENT_FILTER_TYPE_SYSTEM_FLAGS:
    case EVENT_FILTER_TYPE_TRACEHANDLE:
    case EVENT_FILTER_TYPE_PID:
    case EVENT_FILTER_TYPE_EVENT_ID:
    case EVENT_FILTER_TYPE_EVENT_NAME:
        return TRUE;
    default:
        return FALSE;
    }
}

/* EVENT_ENABLE_PROPERTY_SID/TS_ID/STACK_TRACE/PSM_KEY/IGNORE_KEYWORD_0 are
   defined by the SDK, but none can be honoured yet; a set bit is refused
   rather than silently accepted */
#define ETWP_KNOWN_ENABLE_PROPERTIES (EVENT_ENABLE_PROPERTY_SID              | \
                                      EVENT_ENABLE_PROPERTY_TS_ID          | \
                                      EVENT_ENABLE_PROPERTY_STACK_TRACE  | \
                                      EVENT_ENABLE_PROPERTY_PSM_KEY      | \
                                      EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0)

typedef struct _ETWP_ENABLE_FILTERS
{
    LONG RefCount;
    ULONG Count;
    EVENT_FILTER_DESCRIPTOR Descs[ANYSIZE_ARRAY];
    /* the captured filter data follows the descriptors in this allocation */
} ETWP_ENABLE_FILTERS, *PETWP_ENABLE_FILTERS;

static PETWP_ENABLE_FILTERS EtwpReferenceEnableFilters(IN PETWP_ENABLE_FILTERS Filters);
static VOID EtwpReleaseEnableFilters(_In_opt_ PETWP_ENABLE_FILTERS Filters);

typedef struct _ETWP_SESSION_ENABLE
{
    ULONG LoggerId;
    UCHAR Level;
    ULONGLONG MatchAny;
    ULONGLONG MatchAll;
    PETWP_ENABLE_FILTERS Filters;
} ETWP_SESSION_ENABLE, *PETWP_SESSION_ENABLE;

typedef struct _ETWP_PROVIDER
{
    LIST_ENTRY Link;
    GUID Guid;
    LIST_ENTRY Registrations;           /* kernel registrations, under EtwpProviderLock */
    ULONG SessionCount;
    ETWP_SESSION_ENABLE Sessions[ETWP_MAX_SESSIONS];
    /* aggregate, read lock-free */
    volatile LONG IsEnabled;
    volatile UCHAR Level;               /* highest enabled level, ETWP_LEVEL_ALL for level 0 */
    volatile ULONGLONG MatchAny;        /* OR of the sessions' MatchAny, 0 if any session takes all */
} ETWP_PROVIDER, *PETWP_PROVIDER;

typedef struct _ETWP_KREGISTRATION
{
    LIST_ENTRY ProviderLink;
    PETWP_PROVIDER Provider;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
    EX_RUNDOWN_REF Rundown;
} ETWP_KREGISTRATION, *PETWP_KREGISTRATION;

typedef struct _ETWP_BLOCK
{
    PETWP_KREGISTRATION Entry[ETWP_SLOTS_PER_BLOCK];
    PETWP_PROVIDER Provider[ETWP_SLOTS_PER_BLOCK];   /* immortal: readable at any IRQL */
    ULONG Generation[ETWP_SLOTS_PER_BLOCK];
} ETWP_BLOCK, *PETWP_BLOCK;

typedef struct _ETWP_CALLBACK
{
    PETWP_KREGISTRATION Registration;
    ULONG ControlCode;
} ETWP_CALLBACK, *PETWP_CALLBACK;

static EX_PUSH_LOCK EtwpProviderLock;
static LIST_ENTRY EtwpProviders = { &EtwpProviders, &EtwpProviders };
static PETWP_BLOCK EtwpBlocks[ETWP_MAX_BLOCKS];
static KSPIN_LOCK EtwpSlotLock;
static volatile LONG EtwpTelemetryCoverageRound;
static const GUID EtwpNullGuid;

/* --------------------------------------------------------------------------
 * Provider table (PASSIVE_LEVEL, EtwpProviderLock)
 * -------------------------------------------------------------------------- */

static
PETWP_PROVIDER
EtwpFindProviderLocked(
    _In_ LPCGUID Guid)
{
    PLIST_ENTRY Entry;

    for (Entry = EtwpProviders.Flink; Entry != &EtwpProviders; Entry = Entry->Flink)
    {
        PETWP_PROVIDER Provider = CONTAINING_RECORD(Entry, ETWP_PROVIDER, Link);
        if (IsEqualGUID(&Provider->Guid, Guid))
            return Provider;
    }
    return NULL;
}

static
PETWP_PROVIDER
EtwpFindOrCreateProviderLocked(
    _In_ LPCGUID Guid)
{
    PETWP_PROVIDER Provider = EtwpFindProviderLocked(Guid);

    if (Provider)
        return Provider;
    Provider = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Provider), ETWP_PROV_TAG);
    if (!Provider)
        return NULL;
    Provider->Guid = *Guid;
    InitializeListHead(&Provider->Registrations);
    InsertTailList(&EtwpProviders, &Provider->Link);
    return Provider;
}

static
VOID
EtwpRecomputeAggregate(
    _Inout_ PETWP_PROVIDER Provider)
{
    UCHAR Level = 0;
    ULONGLONG MatchAny = 0;
    BOOLEAN AnyKeyword = FALSE;
    ULONG i;

    for (i = 0; i < Provider->SessionCount; i++)
    {
        UCHAR SessionLevel = Provider->Sessions[i].Level ? Provider->Sessions[i].Level : ETWP_LEVEL_ALL;
        if (SessionLevel > Level)
            Level = SessionLevel;
        if (!Provider->Sessions[i].MatchAny)
            AnyKeyword = TRUE;
        MatchAny |= Provider->Sessions[i].MatchAny;
    }
    Provider->Level = Level;
    Provider->MatchAny = AnyKeyword ? 0 : MatchAny;
    InterlockedExchange(&Provider->IsEnabled, Provider->SessionCount != 0);
}

/*
 * Takes a rundown reference on every kernel registration of the provider, so the
 * callbacks can run after the lock is dropped. Returns an array to pass to
 * EtwpRunCallbacks, or NULL when there is none.
 */
static
PETWP_CALLBACK
EtwpCollectCallbacksLocked(
    _In_ PETWP_PROVIDER Provider,
    _In_ ULONG ControlCode,
    _Out_ PULONG Count)
{
    PETWP_CALLBACK Callbacks;
    PLIST_ENTRY Entry;
    ULONG n = 0;

    *Count = 0;
    for (Entry = Provider->Registrations.Flink; Entry != &Provider->Registrations; Entry = Entry->Flink)
        n++;
    if (!n)
        return NULL;
    Callbacks = ExAllocatePoolZero(PagedPool, n * sizeof(*Callbacks), ETWP_PROV_TAG);
    if (!Callbacks)
        return NULL;

    n = 0;
    for (Entry = Provider->Registrations.Flink; Entry != &Provider->Registrations; Entry = Entry->Flink)
    {
        PETWP_KREGISTRATION Registration = CONTAINING_RECORD(Entry, ETWP_KREGISTRATION, ProviderLink);
        if (!Registration->EnableCallback || !ExAcquireRundownProtection(&Registration->Rundown))
            continue;
        Callbacks[n].Registration = Registration;
        Callbacks[n].ControlCode = ControlCode;
        n++;
    }
    *Count = n;
    return Callbacks;
}

/* Calls the collected callbacks with the provider's aggregate, at PASSIVE_LEVEL, no lock held */
static
VOID
EtwpRunCallbacks(
    _In_ PETWP_PROVIDER Provider,
    _In_opt_ PETWP_CALLBACK Callbacks,
    _In_ ULONG Count,
    _In_opt_ PETWP_ENABLE_FILTERS Filters,
    _In_ LPCGUID SourceId)
{
    ULONG i;
    PEVENT_FILTER_DESCRIPTOR FilterData = Filters ? Filters->Descs : NULL;

    for (i = 0; i < Count; i++)
    {
        PETWP_KREGISTRATION Registration = Callbacks[i].Registration;
        UCHAR Level = Provider->Level == ETWP_LEVEL_ALL ? 0 : Provider->Level;
        BOOLEAN Disabling = Callbacks[i].ControlCode == EVENT_CONTROL_CODE_DISABLE_PROVIDER;

        Registration->EnableCallback(SourceId,
                                     Callbacks[i].ControlCode,
                                     Disabling ? 0 : Level,
                                     Disabling ? 0 : Provider->MatchAny,
                                     0,
                                     Disabling ? NULL : FilterData,
                                     Registration->CallbackContext);
        ExReleaseRundownProtection(&Registration->Rundown);
    }
    if (Callbacks)
        ExFreePoolWithTag(Callbacks, ETWP_PROV_TAG);
    /* the callback run's borrow ends here; the session entry keeps its own */
    EtwpReleaseEnableFilters(Filters);
}

static
BOOLEAN
EtwpSessionMatches(
    _In_ const ETWP_SESSION_ENABLE *Session,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    if (Session->Level && Level > Session->Level)
        return FALSE;
    if (!Keyword || !Session->MatchAny)
        return TRUE;
    return (Keyword & Session->MatchAny) && (Keyword & Session->MatchAll) == Session->MatchAll;
}

/* Lock-free first test: can any session want this event? */
static
BOOLEAN
EtwpProviderMayWant(
    _In_ PETWP_PROVIDER Provider,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    ULONGLONG MatchAny;

    if (!Provider->IsEnabled)
        return FALSE;
    if (Level && Level > Provider->Level)
        return FALSE;
    MatchAny = Provider->MatchAny;
    return !Keyword || !MatchAny || (Keyword & MatchAny);
}

/*
 * Writes the event to every session of the provider it matches. Data in
 * DataMode memory. Returns STATUS_SUCCESS also when no session takes it.
 */
static
NTSTATUS
EtwpDeliver(
    _In_ PETWP_PROVIDER Provider,
    _In_ PCEVENT_DESCRIPTOR Descriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData,
    _In_ KPROCESSOR_MODE DataMode,
    _In_ BOOLEAN NarrowCaller)
{
    ULONG Targets[ETWP_MAX_SESSIONS];
    ULONG Count = 0, i;
    NTSTATUS Status = STATUS_SUCCESS, One;

    if (!EtwpProviderMayWant(Provider, Descriptor->Level, Descriptor->Keyword))
        return STATUS_SUCCESS;
    /* the session buffers take a guarded mutex */
    if (KeGetCurrentIrql() > APC_LEVEL)
        return STATUS_SUCCESS;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&EtwpProviderLock);
    for (i = 0; i < Provider->SessionCount; i++)
    {
        if (EtwpSessionMatches(&Provider->Sessions[i], Descriptor->Level, Descriptor->Keyword))
            Targets[Count++] = Provider->Sessions[i].LoggerId;
    }
    ExReleasePushLockShared(&EtwpProviderLock);
    KeLeaveCriticalRegion();

    for (i = 0; i < Count; i++)
    {
        One = EtwpLogModernEvent(Targets[i], &Provider->Guid, Descriptor, ActivityId,
                                 UserDataCount, UserData, DataMode, NarrowCaller);
        /* a payload dropped on a faulting user buffer is not an error; a full session is */
        if (One == STATUS_BUFFER_OVERFLOW)
            Status = One;
    }
    return Status;
}

static
PETWP_ENABLE_FILTERS
EtwpReferenceEnableFilters(IN PETWP_ENABLE_FILTERS Filters)
{
    if (Filters)
        InterlockedIncrement(&Filters->RefCount);
    return Filters;
}

static
VOID
EtwpReleaseEnableFilters(_In_opt_ PETWP_ENABLE_FILTERS Filters)
{
    if (Filters && InterlockedDecrement(&Filters->RefCount) == 0)
        ExFreePoolWithTag(Filters, ETWP_FILTERS_TAG);
}

/* Validates the descriptors and copies them plus every filter blob into one
   kernel-owned allocation: no controller pointers are kept, the descriptor
   Ptr fields are rewritten to the captured data, and the exact sizes are
   preserved. Fails without partial state on any invalid descriptor. */
static
NTSTATUS
EtwpCaptureEnableFilters(
    _In_reads_(DescCount) PEVENT_FILTER_DESCRIPTOR Descs,
    _In_ ULONG DescCount,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PETWP_ENABLE_FILTERS *Captured)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PETWP_ENABLE_FILTERS Store;
    PUCHAR Blobs;
    SIZE_T Total, BlobBytes = 0;
    ULONG i;

    *Captured = NULL;
    for (i = 0; i < DescCount; i++)
    {
        if (Descs[i].Size > MAX_EVENT_FILTER_DATA_SIZE || !EtwpFilterTypeKnown(Descs[i].Type) ||
            (Descs[i].Size && !Descs[i].Ptr))
            return STATUS_INVALID_PARAMETER;
        BlobBytes += Descs[i].Size;
    }

    Total = FIELD_OFFSET(ETWP_ENABLE_FILTERS, Descs) +
            (SIZE_T)DescCount * sizeof(EVENT_FILTER_DESCRIPTOR) + BlobBytes;
    Store = ExAllocatePoolZero(PagedPool, Total, ETWP_FILTERS_TAG);
    if (!Store)
        return STATUS_INSUFFICIENT_RESOURCES;

    Store->RefCount = 1;
    Store->Count = DescCount;
    RtlCopyMemory(Store->Descs, Descs, DescCount * sizeof(EVENT_FILTER_DESCRIPTOR));
    Blobs = (PUCHAR)(Store->Descs + DescCount);
    BlobBytes = 0;
    for (i = 0; i < DescCount; i++)
    {
        if (Store->Descs[i].Size)
        {
            _SEH2_TRY
            {
                if (PreviousMode != KernelMode)
                    ProbeForRead((PVOID)(ULONG_PTR)Descs[i].Ptr, Store->Descs[i].Size, 1);
                RtlCopyMemory(Blobs + BlobBytes, (PVOID)(ULONG_PTR)Descs[i].Ptr, Store->Descs[i].Size);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(Store, ETWP_FILTERS_TAG);
                return Status;
            }
        }
        Store->Descs[i].Ptr = (ULONGLONG)(ULONG_PTR)(Blobs + BlobBytes);
        BlobBytes += Store->Descs[i].Size;
    }

    *Captured = Store;
    return STATUS_SUCCESS;
}

/* Compacts the session array away and drops the entry's filters reference;
   the allocation survives a callback run that borrowed one */
static
VOID
EtwpRemoveSessionEntryLocked(IN PETWP_PROVIDER Provider, IN ULONG Index)
{
    EtwpReleaseEnableFilters(Provider->Sessions[Index].Filters);
    Provider->Sessions[Index] = Provider->Sessions[--Provider->SessionCount];
}

/* --------------------------------------------------------------------------
 * Enable control (NtTraceControl EtwEnableGuidCode)
 * -------------------------------------------------------------------------- */

NTSTATUS
EtwpEnableGuid(
    _In_reads_bytes_opt_(InBufferLen) PVOID InBuffer,
    _In_ ULONG InBufferLen,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    ETW_ENABLE_PARAMETERS Params;
    EVENT_FILTER_DESCRIPTOR DescArray[MAX_EVENT_FILTERS_COUNT];
    PETWP_ENABLE_FILTERS Filters = NULL;
    PETWP_ENABLE_FILTERS RunFilters = NULL;
    PETWP_PROVIDER Provider = NULL;
    PETWP_CALLBACK Callbacks = NULL;
    ULONG Count = 0, CallbackCode = 0, DescBytes = 0, i;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    if (!InBuffer || InBufferLen < sizeof(Params))
        return STATUS_INVALID_PARAMETER;
    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(InBuffer, sizeof(Params), sizeof(ULONG));
        RtlCopyMemory(&Params, InBuffer, sizeof(Params));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (Params.Version != ENABLE_TRACE_PARAMETERS_VERSION &&
        Params.Version != ENABLE_TRACE_PARAMETERS_VERSION_2)
        return STATUS_INVALID_PARAMETER;
    if (Params.ControlCode > EVENT_CONTROL_CODE_CAPTURE_STATE)
        return STATUS_INVALID_PARAMETER;
    if (Params.ControlCode != EVENT_CONTROL_CODE_DISABLE_PROVIDER)
    {
        if (Params.EnableProperty & ~ETWP_KNOWN_ENABLE_PROPERTIES)
            return STATUS_INVALID_PARAMETER;
        /* none of the known properties can be honoured yet (no capture
           infrastructure for SID, TS_ID, stack walks, PSM key or keyword-0
           semantics): refuse instead of silently accepting */
        if (Params.EnableProperty & ETWP_KNOWN_ENABLE_PROPERTIES)
            return STATUS_NOT_SUPPORTED;
    }

    if (Params.EnableFilterDescCount &&
        Params.ControlCode != EVENT_CONTROL_CODE_DISABLE_PROVIDER)
    {
        if (Params.EnableFilterDescCount > MAX_EVENT_FILTERS_COUNT)
            return STATUS_INVALID_PARAMETER;
        DescBytes = Params.EnableFilterDescCount * sizeof(EVENT_FILTER_DESCRIPTOR);
        if (InBufferLen - sizeof(Params) != DescBytes)
            return STATUS_INVALID_PARAMETER;
        _SEH2_TRY
        {
            if (PreviousMode != KernelMode)
                ProbeForRead((PUCHAR)InBuffer + sizeof(Params), DescBytes, sizeof(ULONG));
            RtlCopyMemory(DescArray, (PUCHAR)InBuffer + sizeof(Params), DescBytes);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
        Status = EtwpCaptureEnableFilters(DescArray,
                                          Params.EnableFilterDescCount,
                                          PreviousMode,
                                          &Filters);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else if (InBufferLen != sizeof(Params))
        return STATUS_INVALID_PARAMETER;

    if (Params.ControlCode != EVENT_CONTROL_CODE_DISABLE_PROVIDER && !EtwpLoggerExists(Params.LoggerId))
        return STATUS_WMI_INSTANCE_NOT_FOUND;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&EtwpProviderLock);

    Provider = EtwpFindOrCreateProviderLocked(&Params.ProviderId);
    if (!Provider)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    for (i = 0; i < Provider->SessionCount; i++)
        if (Provider->Sessions[i].LoggerId == Params.LoggerId)
            break;

    switch (Params.ControlCode)
    {
    case EVENT_CONTROL_CODE_ENABLE_PROVIDER:
        if (i == Provider->SessionCount)
        {
            if (Provider->SessionCount == ETWP_MAX_SESSIONS)
            {
                Status = STATUS_TOO_MANY_SESSIONS;
                goto out;
            }
            RtlZeroMemory(&Provider->Sessions[Provider->SessionCount],
                          sizeof(ETWP_SESSION_ENABLE));
            Provider->SessionCount++;
        }
        Provider->Sessions[i].LoggerId = Params.LoggerId;
        Provider->Sessions[i].Level = Params.Level;
        Provider->Sessions[i].MatchAny = Params.MatchAnyKeyword;
        Provider->Sessions[i].MatchAll = Params.MatchAllKeyword;
        EtwpReleaseEnableFilters(Provider->Sessions[i].Filters);
        Provider->Sessions[i].Filters = Filters;   /* takes the captured reference */
        Filters = NULL;
        /*
         * A stop that unlinked the session before this point sweeps the providers
         * only later or already did: test again while the entry is visible, so
         * a dead (and reusable) logger id is never left enabled. A stop after
         * this test finds the entry in its sweep.
         */
        if (!EtwpLoggerExists(Params.LoggerId))
        {
            EtwpRemoveSessionEntryLocked(Provider, i);
            Status = STATUS_WMI_INSTANCE_NOT_FOUND;
            goto out;
        }
        CallbackCode = EVENT_CONTROL_CODE_ENABLE_PROVIDER;
        RunFilters = EtwpReferenceEnableFilters(Provider->Sessions[i].Filters);
        break;

    case EVENT_CONTROL_CODE_DISABLE_PROVIDER:
        if (i == Provider->SessionCount)
            goto out;   /* not enabled for that session: nothing to do */
        EtwpRemoveSessionEntryLocked(Provider, i);
        CallbackCode = Provider->SessionCount ? EVENT_CONTROL_CODE_ENABLE_PROVIDER
                                              : EVENT_CONTROL_CODE_DISABLE_PROVIDER;
        break;

    default: /* capture state: the providers of an enabled session report their state */
        if (i == Provider->SessionCount)
        {
            Status = STATUS_WMI_INSTANCE_NOT_FOUND;
            goto out;
        }
        CallbackCode = EVENT_CONTROL_CODE_CAPTURE_STATE;
        RunFilters = EtwpReferenceEnableFilters(Provider->Sessions[i].Filters);
        break;
    }

    EtwpRecomputeAggregate(Provider);
    Callbacks = EtwpCollectCallbacksLocked(Provider, CallbackCode, &Count);

out:
    ExReleasePushLockExclusive(&EtwpProviderLock);
    KeLeaveCriticalRegion();

    if (NT_SUCCESS(Status) && Provider)
        EtwpRunCallbacks(Provider, Callbacks, Count, RunFilters, &Params.SourceId);
    else
        EtwpReleaseEnableFilters(RunFilters);
    EtwpReleaseEnableFilters(Filters);   /* non-NULL only when never attached */
    return Status;
}

/* A stopped session leaves every provider it had enabled */
VOID
EtwpProvidersLoggerStopped(
    _In_ ULONG LoggerId)
{
    PLIST_ENTRY Entry;

    PAGED_CODE();

    for (;;)
    {
        PETWP_PROVIDER Provider = NULL;
        PETWP_CALLBACK Callbacks = NULL;
        ULONG Count = 0, i;

        KeEnterCriticalRegion();
        ExAcquirePushLockExclusive(&EtwpProviderLock);
        for (Entry = EtwpProviders.Flink; Entry != &EtwpProviders && !Provider; Entry = Entry->Flink)
        {
            PETWP_PROVIDER Candidate = CONTAINING_RECORD(Entry, ETWP_PROVIDER, Link);
            for (i = 0; i < Candidate->SessionCount; i++)
            {
                if (Candidate->Sessions[i].LoggerId == LoggerId)
                {
                    EtwpRemoveSessionEntryLocked(Candidate, i);
                    EtwpRecomputeAggregate(Candidate);
                    Callbacks = EtwpCollectCallbacksLocked(Candidate,
                                                           Candidate->SessionCount ? EVENT_CONTROL_CODE_ENABLE_PROVIDER
                                                                                   : EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                                                           &Count);
                    Provider = Candidate;
                    break;
                }
            }
        }
        ExReleasePushLockExclusive(&EtwpProviderLock);
        KeLeaveCriticalRegion();

        if (!Provider)
            break;
        EtwpRunCallbacks(Provider, Callbacks, Count, NULL, &EtwpNullGuid);
    }
}

/* NtTraceControl EtwQueryEnableCode: would an event of this level and keyword reach a session? */
NTSTATUS
EtwpQueryEnable(
    _In_reads_bytes_opt_(InBufferLen) PVOID InBuffer,
    _In_ ULONG InBufferLen,
    _Out_writes_bytes_opt_(OutBufferLen) PVOID OutBuffer,
    _In_ ULONG OutBufferLen,
    _Out_opt_ PULONG ReturnSize,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    ETW_QUERY_ENABLE Query;
    PETWP_PROVIDER Provider;
    GUID Guid;
    ULONG Enabled = 0, i;
    NTSTATUS Status;

    PAGED_CODE();

    if (!InBuffer || InBufferLen != sizeof(Query) || !OutBuffer || OutBufferLen < sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            ProbeForRead(InBuffer, sizeof(Query), sizeof(ULONG));
            ProbeForWrite(OutBuffer, sizeof(ULONG), sizeof(ULONG));
            if (ReturnSize)
                ProbeForWriteUlong(ReturnSize);
        }
        RtlCopyMemory(&Query, InBuffer, sizeof(Query));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    /* Only for the caller's own registration: no one learns another provider's state */
    Status = EtwpGuidFromRegistrationHandle((HANDLE)(ULONG_PTR)Query.RegistrationHandle, PreviousMode, &Guid);
    if (!NT_SUCCESS(Status))
        return Status;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&EtwpProviderLock);
    Provider = EtwpFindProviderLocked(&Guid);
    if (Provider)
    {
        for (i = 0; i < Provider->SessionCount && !Enabled; i++)
            Enabled = EtwpSessionMatches(&Provider->Sessions[i], Query.Level, Query.Keyword);
    }
    ExReleasePushLockShared(&EtwpProviderLock);
    KeLeaveCriticalRegion();

    _SEH2_TRY
    {
        *(PULONG)OutBuffer = Enabled;
        if (ReturnSize)
            *ReturnSize = sizeof(ULONG);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;
    return STATUS_SUCCESS;
}

/* NtTraceEvent(ETW_NT_FLAGS_TRACE_EVENT): a modern event from a user-mode provider */
NTSTATUS
EtwpTraceUserEvent(
    _In_opt_ HANDLE RegistrationHandle,
    _In_ ULONG PacketSize,
    _In_ PVOID Packet,
    _In_ KPROCESSOR_MODE PreviousMode)
{
    EVENT_DATA_DESCRIPTOR Data[ETW_MAX_EVENT_DATA_DESCRIPTORS];
    EVENT_DATA_DESCRIPTOR Captured;
    ETW_USER_EVENT_PACKET Header;
    PETWP_PROVIDER Provider;
    PUCHAR Copy = NULL;
    ULONG Total = 0, i;
    GUID Guid;
    NTSTATUS Status;

    PAGED_CODE();

    if (!Packet || PacketSize < FIELD_OFFSET(ETW_USER_EVENT_PACKET, UserData))
        return STATUS_INVALID_PARAMETER;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
            ProbeForRead(Packet, PacketSize, sizeof(ULONG));
        RtlCopyMemory(&Header, Packet, FIELD_OFFSET(ETW_USER_EVENT_PACKET, UserData));
        if (Header.UserDataCount > ETW_MAX_EVENT_DATA_DESCRIPTORS ||
            PacketSize < FIELD_OFFSET(ETW_USER_EVENT_PACKET, UserData) + Header.UserDataCount * sizeof(EVENT_DATA_DESCRIPTOR))
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }
        /* the descriptors are captured once; the data they point to is probed on copy */
        RtlCopyMemory(Data, ((PETW_USER_EVENT_PACKET)Packet)->UserData,
                      Header.UserDataCount * sizeof(EVENT_DATA_DESCRIPTOR));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    Status = EtwpGuidFromRegistrationHandle(RegistrationHandle, PreviousMode, &Guid);
    if (!NT_SUCCESS(Status))
        return Status;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&EtwpProviderLock);
    Provider = EtwpFindProviderLocked(&Guid);
    ExReleasePushLockShared(&EtwpProviderLock);
    KeLeaveCriticalRegion();
    if (!Provider)
        return STATUS_SUCCESS;   /* no session ever enabled it */
    if (!EtwpProviderMayWant(Provider, Header.Descriptor.Level, Header.Descriptor.Keyword))
        return STATUS_SUCCESS;

    /*
     * The data is copied from user memory into one kernel buffer now, before any
     * session lock: a slow page-in must not hold up the other writers of a session.
     */
    for (i = 0; i < Header.UserDataCount; i++)
    {
        if (Data[i].Size > MAXUSHORT || Total + Data[i].Size > MAXUSHORT)
            return STATUS_BUFFER_OVERFLOW;
        Total += Data[i].Size;
    }
    if (Total)
    {
        Copy = ExAllocatePoolWithTag(PagedPool, Total, ETWP_PROV_TAG);
        if (!Copy)
            return STATUS_INSUFFICIENT_RESOURCES;
        _SEH2_TRY
        {
            ULONG Offset = 0;
            for (i = 0; i < Header.UserDataCount; i++)
            {
                if (!Data[i].Size)
                    continue;
                if (PreviousMode != KernelMode)
                    ProbeForRead((PVOID)(ULONG_PTR)Data[i].Ptr, Data[i].Size, sizeof(UCHAR));
                RtlCopyMemory(Copy + Offset, (PVOID)(ULONG_PTR)Data[i].Ptr, Data[i].Size);
                Offset += Data[i].Size;
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            ExFreePoolWithTag(Copy, ETWP_PROV_TAG);
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }
    Captured.Ptr = (ULONGLONG)(ULONG_PTR)Copy;
    Captured.Size = Total;
    Captured.Reserved = 0;

    Status = EtwpDeliver(Provider, &Header.Descriptor,
                         (Header.Flags & ETW_USER_EVENT_ACTIVITY_ID) ? &Header.ActivityId : NULL,
                         Total ? 1 : 0, &Captured, KernelMode,
                         sizeof(PVOID) != sizeof(ULONG) &&
                         PreviousMode != KernelMode && PsGetCurrentProcessWow64Process() != NULL);
    if (Copy)
        ExFreePoolWithTag(Copy, ETWP_PROV_TAG);
    return Status;
}

/* --------------------------------------------------------------------------
 * Kernel provider API
 * -------------------------------------------------------------------------- */

/* The slot of a live handle, or NULL; at any IRQL, never dereferences the registration */
static
PETWP_PROVIDER
EtwpProviderFromHandle(
    _In_ REGHANDLE RegHandle)
{
    ULONG Index = (ULONG)(RegHandle & 0xFFFFFFFF);
    ULONG Generation = (ULONG)(RegHandle >> 32);
    PETWP_BLOCK Block;
    PETWP_PROVIDER Provider;

    if (Index == 0 || Index > ETWP_MAX_BLOCKS * ETWP_SLOTS_PER_BLOCK)
        return NULL;
    Index--;
    Block = ReadPointerNoFence((PVOID volatile *)&EtwpBlocks[Index / ETWP_SLOTS_PER_BLOCK]);
    if (!Block)
        return NULL;
    KeMemoryBarrier();
    /* The generation is read before and after the slot, with barriers: on weakly
     * ordered processors a reused slot's provider must not pair with the old
     * generation */
    if (ReadULongNoFence(&Block->Generation[Index % ETWP_SLOTS_PER_BLOCK]) != Generation)
        return NULL;
    KeMemoryBarrier();
    if (!ReadPointerNoFence((PVOID volatile *)&Block->Entry[Index % ETWP_SLOTS_PER_BLOCK]))
        return NULL;
    Provider = ReadPointerNoFence((PVOID volatile *)&Block->Provider[Index % ETWP_SLOTS_PER_BLOCK]);
    KeMemoryBarrier();
    if (ReadULongNoFence(&Block->Generation[Index % ETWP_SLOTS_PER_BLOCK]) != Generation)
        return NULL;
    return Provider;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_KREGISTRATION Registration;
    PETWP_PROVIDER Provider;
    PETWP_BLOCK NewBlock = NULL, Block;
    PETWP_CALLBACK Callbacks = NULL;
    ULONG BlockIndex, Slot, Count = 0;
    KIRQL OldIrql;

    PAGED_CODE();

    if (!ProviderId || !RegHandle)
        return STATUS_INVALID_PARAMETER;
    *RegHandle = 0;

    Registration = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Registration), ETWP_PROV_TAG);
    if (!Registration)
        return STATUS_INSUFFICIENT_RESOURCES;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;
    ExInitializeRundownProtection(&Registration->Rundown);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&EtwpProviderLock);
    Provider = EtwpFindOrCreateProviderLocked(ProviderId);
    if (Provider)
    {
        Registration->Provider = Provider;
        InsertTailList(&Provider->Registrations, &Registration->ProviderLink);
    }
    ExReleasePushLockExclusive(&EtwpProviderLock);
    KeLeaveCriticalRegion();
    if (!Provider)
    {
        ExFreePoolWithTag(Registration, ETWP_PROV_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (;;)
    {
        KeAcquireSpinLock(&EtwpSlotLock, &OldIrql);
        for (BlockIndex = 0; BlockIndex < ETWP_MAX_BLOCKS; BlockIndex++)
        {
            Block = EtwpBlocks[BlockIndex];
            if (!Block)
            {
                if (!NewBlock)
                    break;
                Block = NewBlock;
                NewBlock = NULL;
                WritePointerRelease((PVOID volatile *)&EtwpBlocks[BlockIndex], Block);
            }
            for (Slot = 0; Slot < ETWP_SLOTS_PER_BLOCK; Slot++)
            {
                if (!Block->Entry[Slot])
                {
                    WritePointerRelease((PVOID volatile *)&Block->Provider[Slot], Provider);
                    WritePointerRelease((PVOID volatile *)&Block->Entry[Slot], Registration);
                    *RegHandle = ((REGHANDLE)Block->Generation[Slot] << 32) |
                                 (BlockIndex * ETWP_SLOTS_PER_BLOCK + Slot + 1);
                    KeReleaseSpinLock(&EtwpSlotLock, OldIrql);
                    if (NewBlock)
                        ExFreePoolWithTag(NewBlock, ETWP_PROV_TAG);
                    goto registered;
                }
            }
        }
        KeReleaseSpinLock(&EtwpSlotLock, OldIrql);
        if (BlockIndex == ETWP_MAX_BLOCKS ||
            !(NewBlock = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*NewBlock), ETWP_PROV_TAG)))
            break;
    }

    /* no slot: undo */
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&EtwpProviderLock);
    RemoveEntryList(&Registration->ProviderLink);
    ExReleasePushLockExclusive(&EtwpProviderLock);
    KeLeaveCriticalRegion();
    /* an enable may have collected it meanwhile and be inside its callback */
    ExWaitForRundownProtectionRelease(&Registration->Rundown);
    ExFreePoolWithTag(Registration, ETWP_PROV_TAG);
    return STATUS_INSUFFICIENT_RESOURCES;

registered:
    /* A session that enabled the provider before it registered is reported now */
    if (EnableCallback)
    {
        KeEnterCriticalRegion();
        ExAcquirePushLockShared(&EtwpProviderLock);
        if (Provider->SessionCount && ExAcquireRundownProtection(&Registration->Rundown))
        {
            Callbacks = ExAllocatePoolZero(PagedPool, sizeof(*Callbacks), ETWP_PROV_TAG);
            if (Callbacks)
            {
                Callbacks->Registration = Registration;
                Callbacks->ControlCode = EVENT_CONTROL_CODE_ENABLE_PROVIDER;
                Count = 1;
            }
            else
            {
                ExReleaseRundownProtection(&Registration->Rundown);
            }
        }
        ExReleasePushLockShared(&EtwpProviderLock);
        KeLeaveCriticalRegion();
        if (Count)
            EtwpRunCallbacks(Provider, Callbacks, Count, NULL, &EtwpNullGuid);
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    ULONG Index = (ULONG)(RegHandle & 0xFFFFFFFF);
    PETWP_KREGISTRATION Registration;
    PETWP_BLOCK Block;
    KIRQL OldIrql;

    PAGED_CODE();

    if (Index == 0 || Index > ETWP_MAX_BLOCKS * ETWP_SLOTS_PER_BLOCK)
        return STATUS_INVALID_HANDLE;
    Index--;

    KeAcquireSpinLock(&EtwpSlotLock, &OldIrql);
    Block = EtwpBlocks[Index / ETWP_SLOTS_PER_BLOCK];
    if (!Block || !(Registration = Block->Entry[Index % ETWP_SLOTS_PER_BLOCK]) ||
        Block->Generation[Index % ETWP_SLOTS_PER_BLOCK] != (ULONG)(RegHandle >> 32))
    {
        KeReleaseSpinLock(&EtwpSlotLock, OldIrql);
        return STATUS_INVALID_HANDLE;
    }
    Block->Generation[Index % ETWP_SLOTS_PER_BLOCK]++;
    WritePointerRelease((PVOID volatile *)&Block->Entry[Index % ETWP_SLOTS_PER_BLOCK], NULL);
    KeReleaseSpinLock(&EtwpSlotLock, OldIrql);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&EtwpProviderLock);
    RemoveEntryList(&Registration->ProviderLink);
    ExReleasePushLockExclusive(&EtwpProviderLock);
    KeLeaveCriticalRegion();

    /* a callback that is running finishes first */
    ExWaitForRundownProtectionRelease(&Registration->Rundown);
    ExFreePoolWithTag(Registration, ETWP_PROV_TAG);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    PETWP_PROVIDER Provider = EtwpProviderFromHandle(RegHandle);

    if (!Provider || !EventDescriptor)
        return FALSE;
    return EtwpProviderMayWant(Provider, EventDescriptor->Level, EventDescriptor->Keyword);
}

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    PETWP_PROVIDER Provider = EtwpProviderFromHandle(RegHandle);

    return Provider && EtwpProviderMayWant(Provider, Level, Keyword);
}

NTSTATUS
NTAPI
EtwActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    if (!ActivityId)
        return STATUS_INVALID_PARAMETER;

    switch (ControlCode)
    {
    case EVENT_ACTIVITY_CTRL_CREATE_ID:
    case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
        return ExUuidCreate(ActivityId);
    case EVENT_ACTIVITY_CTRL_GET_ID:
    case EVENT_ACTIVITY_CTRL_GET_SET_ID:
        /* no per-thread activity id is kept yet */
        RtlZeroMemory(ActivityId, sizeof(*ActivityId));
        return STATUS_SUCCESS;
    case EVENT_ACTIVITY_CTRL_SET_ID:
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    PETWP_PROVIDER Provider;

    UNREFERENCED_PARAMETER(RelatedActivityId);

    if (!EventDescriptor || UserDataCount > ETW_MAX_EVENT_DATA_DESCRIPTORS || (UserDataCount && !UserData))
        return STATUS_INVALID_PARAMETER;
    if (!(Provider = EtwpProviderFromHandle(RegHandle)))
        return STATUS_INVALID_HANDLE;

    return EtwpDeliver(Provider, EventDescriptor, ActivityId, UserDataCount, UserData, KernelMode, FALSE);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, NULL, UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);

    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

/* A string event: the descriptor carries only level and keyword; the data is the string */
_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_opt_ LPCGUID ActivityId,
    _In_ PCWSTR String)
{
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data;
    PETWP_PROVIDER Provider;
    SIZE_T Length;

    if (!String)
        return STATUS_INVALID_PARAMETER;
    if (!(Provider = EtwpProviderFromHandle(RegHandle)))
        return STATUS_INVALID_HANDLE;

    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Descriptor.Level = Level;
    Descriptor.Keyword = Keyword;
    Length = (wcslen(String) + 1) * sizeof(WCHAR);
    if (Length > MAXUSHORT)
        return STATUS_INVALID_PARAMETER;
    Data.Ptr = (ULONGLONG)(ULONG_PTR)String;
    Data.Size = (ULONG)Length;
    Data.Reserved = 0;

    return EtwpDeliver(Provider, &Descriptor, ActivityId, 1, &Data, KernelMode, FALSE);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_opt_ PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(InformationClass);
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);

    /* provider traits are accepted and not used yet */
    return EtwpProviderFromHandle(RegHandle) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

/*
 * Classic (WPP) providers. They are registered like modern ones; their
 * callback is a WMI-style request routine with another signature, so it is not
 * called: WPP tracing stays off for them, as before.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwRegisterClassicProvider(
    _In_ LPCGUID ProviderGuid,
    _In_ ULONG Type,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(EnableCallback);
    UNREFERENCED_PARAMETER(CallbackContext);

    return EtwRegister(ProviderGuid, NULL, NULL, RegHandle);
}

NTSTATUS
NTAPI
EtwTelemetryCoverageReport(
    _In_ PVOID CoverageData)
{
    if (!CoverageData)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&EtwpTelemetryCoverageRound);
    return STATUS_SUCCESS;
}
