/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Silo context slots and server silo monitors (host-silo backed)
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* CONSTANTS *****************************************************************/

#define PS_SILO_CONTEXT_SLOTS_MAX 128
#define PS_SILO_CONTEXT_WORDS (PS_SILO_CONTEXT_SLOTS_MAX / (sizeof(ULONG) * 8))

#define PS_SILO_HEADER_SIZE ((sizeof(PS_SILO_CONTEXT_HEADER) + \
                              MEMORY_ALLOCATION_ALIGNMENT - 1) & \
                             ~(MEMORY_ALLOCATION_ALIGNMENT - 1))

#define TAG_SILO_CONTEXT 'OLIS'
#define TAG_SILO_MONITOR 'MLIS'
#define TAG_SILO_NAME    'NLIS'

/* TYPES *********************************************************************/

typedef struct _PS_SILO_CONTEXT_HEADER
{
    POOL_TYPE PoolType;
    ULONG Size;
    volatile LONG RefCount;
    SILO_CONTEXT_CLEANUP_CALLBACK CleanupCallback;
} PS_SILO_CONTEXT_HEADER, *PPS_SILO_CONTEXT_HEADER;

struct _SILO_MONITOR
{
    UNICODE_STRING ComponentName;
    SILO_MONITOR_CREATE_CALLBACK CreateCallback;
    SILO_MONITOR_TERMINATE_CALLBACK TerminateCallback;
    BOOLEAN MonitorHost;
    BOOLEAN Started;
    BOOLEAN HostCreated;
    ULONG ContextSlot;
    LIST_ENTRY Link;
};

typedef struct _SILO_MONITOR SILO_MONITOR;

/* GLOBALS *******************************************************************/

static FAST_MUTEX PsSiloContextLock;
static ULONG PsSiloContextSlotBitmap[PS_SILO_CONTEXT_WORDS];
static PVOID PsHostSiloContextTable[PS_SILO_CONTEXT_SLOTS_MAX];
static ULONG PsHostSiloContextSlotCount;
static ULONG PsHostSiloContextPermanent[PS_SILO_CONTEXT_WORDS];
static LIST_ENTRY PsSiloMonitorList;

/* PRIVATE HELPERS ***********************************************************/

static PPS_SILO_CONTEXT_HEADER
PsSiloContextHeader(_In_ PVOID SiloContext)
{
    return (PPS_SILO_CONTEXT_HEADER)((PUCHAR)SiloContext - PS_SILO_HEADER_SIZE);
}

static BOOLEAN
PsSiloContextSlotAllocated(_In_ ULONG ContextSlot)
{
    return ((PsSiloContextSlotBitmap[ContextSlot / 32] >> (ContextSlot % 32)) & 1) != 0;
}

/* PUBLIC API ****************************************************************/

NTSTATUS
NTAPI
PsAllocSiloContextSlot(
    _In_ ULONG_PTR Reserved,
    _Out_ PULONG ReturnedContextSlot)
{
    ULONG i;

    PAGED_CODE();

    if ((Reserved != 0) || (ReturnedContextSlot == NULL))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);
    for (i = 0; i < PS_SILO_CONTEXT_SLOTS_MAX; i++)
    {
        if (!PsSiloContextSlotAllocated(i))
        {
            PsSiloContextSlotBitmap[i / 32] |= (1UL << (i % 32));
            *ReturnedContextSlot = i;
            ExReleaseFastMutex(&PsSiloContextLock);
            return STATUS_SUCCESS;
        }
    }
    ExReleaseFastMutex(&PsSiloContextLock);

    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
NTAPI
PsFreeSiloContextSlot(
    _In_ ULONG ContextSlot)
{
    PVOID StaleContext = NULL;

    PAGED_CODE();

    if (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX)
        return;

    ExAcquireFastMutex(&PsSiloContextLock);
    PsSiloContextSlotBitmap[ContextSlot / 32] &= ~(1UL << (ContextSlot % 32));
    if (ContextSlot < PsHostSiloContextSlotCount)
    {
        StaleContext = PsHostSiloContextTable[ContextSlot];
        PsHostSiloContextTable[ContextSlot] = NULL;
        PsHostSiloContextPermanent[ContextSlot / 32] &= ~(1UL << (ContextSlot % 32));
    }
    ExReleaseFastMutex(&PsSiloContextLock);

    if (StaleContext != NULL)
        PsDereferenceSiloContext(StaleContext);
}

NTSTATUS
NTAPI
PsCreateSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG Size,
    _In_ POOL_TYPE PoolType,
    _In_opt_ SILO_CONTEXT_CLEANUP_CALLBACK ContextCleanupCallback,
    _Out_ PVOID *ReturnedSiloContext)
{
    PPS_SILO_CONTEXT_HEADER Header;

    PAGED_CODE();

    if (Silo != NULL)
        return STATUS_NOT_SUPPORTED;
    if (ReturnedSiloContext == NULL)
        return STATUS_INVALID_PARAMETER;
    if ((PoolType != NonPagedPoolNx) && (PoolType != PagedPool))
        return STATUS_INVALID_PARAMETER;

    Header = ExAllocatePoolZero(PoolType, PS_SILO_HEADER_SIZE + Size, TAG_SILO_CONTEXT);
    if (Header == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header->PoolType = PoolType;
    Header->Size = Size;
    Header->RefCount = 1;
    Header->CleanupCallback = ContextCleanupCallback;

    *ReturnedSiloContext = (PUCHAR)Header + PS_SILO_HEADER_SIZE;
    return STATUS_SUCCESS;
}

VOID
NTAPI
PsReferenceSiloContext(
    _In_ PVOID SiloContext)
{
    InterlockedIncrement(&PsSiloContextHeader(SiloContext)->RefCount);
}

VOID
NTAPI
PsDereferenceSiloContext(
    _In_ PVOID SiloContext)
{
    PPS_SILO_CONTEXT_HEADER Header = PsSiloContextHeader(SiloContext);

    if (InterlockedDecrement(&Header->RefCount) == 0)
    {
        if (Header->CleanupCallback != NULL)
            Header->CleanupCallback(SiloContext);
        ExFreePoolWithTag(Header, TAG_SILO_CONTEXT);
    }
}

NTSTATUS
NTAPI
PsInsertSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot,
    _In_ PVOID SiloContext)
{
    PAGED_CODE();

    if (Silo != NULL)
        return STATUS_NOT_SUPPORTED;
    if ((SiloContext == NULL) || (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);

    if (ContextSlot >= PsHostSiloContextSlotCount)
        PsHostSiloContextSlotCount = ContextSlot + 1;

    if (PsHostSiloContextTable[ContextSlot] != NULL)
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        return STATUS_NOT_SUPPORTED;
    }

    PsHostSiloContextTable[ContextSlot] = SiloContext;
    InterlockedIncrement(&PsSiloContextHeader(SiloContext)->RefCount);

    ExReleaseFastMutex(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsGetSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot,
    _Out_ PVOID *ReturnedSiloContext)
{
    PVOID Context;

    PAGED_CODE();

    if (Silo != NULL)
        return STATUS_NOT_SUPPORTED;
    if ((ReturnedSiloContext == NULL) ||
        (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);

    if ((ContextSlot >= PsHostSiloContextSlotCount) ||
        ((Context = PsHostSiloContextTable[ContextSlot]) == NULL))
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_FOUND;
    }

    InterlockedIncrement(&PsSiloContextHeader(Context)->RefCount);
    *ReturnedSiloContext = Context;

    ExReleaseFastMutex(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsMakeSiloContextPermanent(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot)
{
    PAGED_CODE();

    if (Silo != NULL)
        return STATUS_NOT_SUPPORTED;
    if ((ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);

    if ((ContextSlot >= PsHostSiloContextSlotCount) ||
        (PsHostSiloContextTable[ContextSlot] == NULL))
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        return STATUS_NOT_FOUND;
    }

    PsHostSiloContextPermanent[ContextSlot / 32] |= (1UL << (ContextSlot % 32));

    ExReleaseFastMutex(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsInsertPermanentSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot,
    _In_ PVOID SiloContext)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = PsInsertSiloContext(Silo, ContextSlot, SiloContext);
    if (NT_SUCCESS(Status))
        PsMakeSiloContextPermanent(Silo, ContextSlot);

    return Status;
}

NTSTATUS
NTAPI
PsGetPermanentSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot,
    _Out_ PVOID *ReturnedSiloContext)
{
    PVOID Context;

    PAGED_CODE();

    if (Silo != NULL)
        return STATUS_NOT_SUPPORTED;
    if ((ReturnedSiloContext == NULL) ||
        (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);

    if ((ContextSlot >= PsHostSiloContextSlotCount) ||
        ((Context = PsHostSiloContextTable[ContextSlot]) == NULL))
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_FOUND;
    }

    if (((PsHostSiloContextPermanent[ContextSlot / 32] >> (ContextSlot % 32)) & 1) == 0)
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_SUPPORTED;
    }

    *ReturnedSiloContext = Context;

    ExReleaseFastMutex(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsRegisterSiloMonitor(
    _In_ PSILO_MONITOR_REGISTRATION Registration,
    _Out_ PSILO_MONITOR *ReturnedMonitor)
{
    PSILO_MONITOR Monitor;
    ULONG ContextSlot;
    NTSTATUS Status;

    PAGED_CODE();

    if ((Registration == NULL) || (ReturnedMonitor == NULL))
        return STATUS_INVALID_PARAMETER;
    if ((Registration->Version != SILO_MONITOR_REGISTRATION_VERSION) ||
        (Registration->ComponentName == NULL) ||
        (Registration->ComponentName->Buffer == NULL) ||
        (Registration->ComponentName->Length == 0) ||
        (Registration->ComponentName->Length > Registration->ComponentName->MaximumLength) ||
        (Registration->TerminateCallback == NULL))
        return STATUS_INVALID_PARAMETER;
    if (PsGetCurrentServerSilo() != NULL)
        return STATUS_PRIVILEGE_NOT_HELD;

    Status = PsAllocSiloContextSlot(0, &ContextSlot);
    if (!NT_SUCCESS(Status))
        return Status;

    Monitor = ExAllocatePoolZero(PagedPool, sizeof(SILO_MONITOR), TAG_SILO_MONITOR);
    if (Monitor == NULL)
    {
        PsFreeSiloContextSlot(ContextSlot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Monitor->ComponentName.Length = Registration->ComponentName->Length;
    Monitor->ComponentName.MaximumLength = Registration->ComponentName->Length;
    Monitor->ComponentName.Buffer = ExAllocatePoolZero(PagedPool,
                                                       Registration->ComponentName->Length,
                                                       TAG_SILO_NAME);
    if (Monitor->ComponentName.Buffer == NULL)
    {
        ExFreePoolWithTag(Monitor, TAG_SILO_MONITOR);
        PsFreeSiloContextSlot(ContextSlot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Monitor->ComponentName.Buffer,
                  Registration->ComponentName->Buffer,
                  Registration->ComponentName->Length);

    Monitor->CreateCallback = Registration->CreateCallback;
    Monitor->TerminateCallback = Registration->TerminateCallback;
    Monitor->MonitorHost = Registration->MonitorHost;
    Monitor->ContextSlot = ContextSlot;

    ExAcquireFastMutex(&PsSiloContextLock);
    InsertTailList(&PsSiloMonitorList, &Monitor->Link);
    ExReleaseFastMutex(&PsSiloContextLock);

    *ReturnedMonitor = Monitor;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsStartSiloMonitor(
    _In_ PSILO_MONITOR SiloMonitor)
{
    BOOLEAN NotifyHost;
    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    if (SiloMonitor == NULL)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&PsSiloContextLock);
    if (SiloMonitor->Started)
    {
        ExReleaseFastMutex(&PsSiloContextLock);
        return STATUS_SUCCESS;
    }
    SiloMonitor->Started = TRUE;
    NotifyHost = SiloMonitor->MonitorHost && (SiloMonitor->CreateCallback != NULL);
    ExReleaseFastMutex(&PsSiloContextLock);

    if (NotifyHost)
    {
        Status = SiloMonitor->CreateCallback(NULL);
        if (NT_SUCCESS(Status))
        {
            ExAcquireFastMutex(&PsSiloContextLock);
            SiloMonitor->HostCreated = TRUE;
            ExReleaseFastMutex(&PsSiloContextLock);
        }
    }

    return Status;
}

VOID
NTAPI
PsUnregisterSiloMonitor(
    _In_ PSILO_MONITOR SiloMonitor)
{
    BOOLEAN NotifyHost;

    PAGED_CODE();

    if (SiloMonitor == NULL)
        return;

    ExAcquireFastMutex(&PsSiloContextLock);
    NotifyHost = SiloMonitor->HostCreated;
    SiloMonitor->HostCreated = FALSE;
    RemoveEntryList(&SiloMonitor->Link);
    ExReleaseFastMutex(&PsSiloContextLock);

    if (NotifyHost)
        SiloMonitor->TerminateCallback(NULL);

    PsFreeSiloContextSlot(SiloMonitor->ContextSlot);
    ExFreePoolWithTag(SiloMonitor->ComponentName.Buffer, TAG_SILO_NAME);
    ExFreePoolWithTag(SiloMonitor, TAG_SILO_MONITOR);
}

ULONG
NTAPI
PsGetSiloMonitorContextSlot(
    _In_ PSILO_MONITOR SiloMonitor)
{
    PAGED_CODE();

    if (SiloMonitor == NULL)
        return (ULONG)-1;

    return SiloMonitor->ContextSlot;
}

PESILO
NTAPI
PsGetCurrentSilo(VOID)
{
    return NULL;
}

PESILO
NTAPI
PsGetParentSilo(
    _In_opt_ PESILO Silo)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Silo);
    return NULL;
}

PESILO
NTAPI
PsGetEffectiveServerSilo(
    _In_ PESILO Silo)
{
    UNREFERENCED_PARAMETER(Silo);
    return NULL;
}

PESILO
NTAPI
PsGetThreadServerSilo(
    _In_ PETHREAD Thread)
{
    UNREFERENCED_PARAMETER(Thread);
    return NULL;
}

/* INITIALIZATION ************************************************************/

VOID
NTAPI
PsSiloInitialize(VOID)
{
    ExInitializeFastMutex(&PsSiloContextLock);
    InitializeListHead(&PsSiloMonitorList);
}
