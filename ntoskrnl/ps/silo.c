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

#define TAG_SILO_CONTEXT 'OLIS'
#define TAG_SILO_MONITOR 'MLIS'
#define TAG_SILO_NAME    'NLIS'

/* TYPES *********************************************************************/

typedef struct _PS_SILO_CONTEXT_HEADER
{
    POOL_TYPE PoolType;
    ULONG Size;
    volatile LONG RefCount;
    PSILO_CONTEXT_CLEANUP_CALLBACK CleanupCallback;
} PS_SILO_CONTEXT_HEADER, *PPS_SILO_CONTEXT_HEADER;

struct _SILO_MONITOR
{
    UNICODE_STRING ComponentName;
    SILO_MONITOR_CREATE_CALLBACK CreateCallback;
    SILO_MONITOR_TERMINATE_CALLBACK TerminateCallback;
    BOOLEAN MonitorHost;
    BOOLEAN Started;
    ULONG ContextSlot;
    LIST_ENTRY Link;
};

/* GLOBALS *******************************************************************/

static FAST_MUTEX PsSiloContextLock;
static ULONG PsSiloContextSlotBitmap[PS_SILO_CONTEXT_WORDS];
static PVOID PsHostSiloContextTable[PS_SILO_CONTEXT_SLOTS_MAX];
static ULONG PsHostSiloContextSlotCount;
static ULONG PsHostSiloContextPermanent[PS_SILO_CONTEXT_WORDS];
static LIST_ENTRY PsSiloMonitorList;

/* PRIVATE HELPERS ***********************************************************/

static PVOID *
PsSiloContextStorage(
    _In_ PESILO Silo,
    _Out_ PULONG *SlotCount,
    _Out_ ULONG **PermanentMap)
{
    if (Silo == NULL)
    {
        *SlotCount = &PsHostSiloContextSlotCount;
        *PermanentMap = PsHostSiloContextPermanent;
        return PsHostSiloContextTable;
    }

    *SlotCount = &((PEJOB)Silo)->SiloContextSlotCount;
    *PermanentMap = ((PEJOB)Silo)->SiloContextPermanent;
    return ((PEJOB)Silo)->SiloContextTable;
}

static BOOLEAN
PsSiloContextSlotAllocated(_In_ ULONG ContextSlot)
{
    return ((PsSiloContextSlotBitmap[ContextSlot / 32] >> (ContextSlot % 32)) & 1) != 0;
}

static NTSTATUS
PsSiloContextGrowTable(
    _In_ PESILO Silo,
    _Inout_ PVOID **Table,
    _Inout_ PULONG *SlotCount,
    _In_ ULONG NewCount)
{
    PVOID *NewTable;
    ULONG OldCount, CopyCount;

    if (*Table == PsHostSiloContextTable)
    {
        **SlotCount = NewCount;
        return STATUS_SUCCESS;
    }

    OldCount = **SlotCount;
    NewTable = ExAllocatePoolZero(PagedPool, NewCount * sizeof(PVOID), TAG_SILO_CONTEXT);
    if (NewTable == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    CopyCount = min(OldCount, NewCount);
    RtlCopyMemory(NewTable, **Table, CopyCount * sizeof(PVOID));
    if (**Table != NULL)
        ExFreePoolWithTag(**Table, TAG_SILO_CONTEXT);

    ((PEJOB)Silo)->SiloContextTable = NewTable;
    ((PEJOB)Silo)->SiloContextSlotCount = NewCount;
    *Table = NewTable;
    *SlotCount = &((PEJOB)Silo)->SiloContextSlotCount;
    return STATUS_SUCCESS;
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

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);
    for (i = 0; i < PS_SILO_CONTEXT_SLOTS_MAX; i++)
    {
        if (!PsSiloContextSlotAllocated(i))
        {
            PsSiloContextSlotBitmap[i / 32] |= (1UL << (i % 32));
            *ReturnedContextSlot = i;
            ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
            return STATUS_SUCCESS;
        }
    }
    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);

    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
NTAPI
PsFreeSiloContextSlot(
    _In_ ULONG ContextSlot)
{
    PAGED_CODE();

    if (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX)
        return;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);
    PsSiloContextSlotBitmap[ContextSlot / 32] &= ~(1UL << (ContextSlot % 32));
    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
}

NTSTATUS
NTAPI
PsCreateSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG Size,
    _In_ POOL_TYPE PoolType,
    _In_opt_ PSILO_CONTEXT_CLEANUP_CALLBACK ContextCleanupCallback,
    _Out_ PVOID *ReturnedSiloContext)
{
    PPS_SILO_CONTEXT_HEADER Header;

    PAGED_CODE();

    if (ReturnedSiloContext == NULL)
        return STATUS_INVALID_PARAMETER;
    if ((PoolType != NonPagedPoolNx) && (PoolType != PagedPool))
        return STATUS_INVALID_PARAMETER;

    Header = ExAllocatePoolZero(PoolType,
                                sizeof(PS_SILO_CONTEXT_HEADER) + Size,
                                TAG_SILO_CONTEXT);
    if (Header == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header->PoolType = PoolType;
    Header->Size = Size;
    Header->RefCount = 1;
    Header->CleanupCallback = ContextCleanupCallback;

    *ReturnedSiloContext = Header + 1;
    return STATUS_SUCCESS;
}

VOID
NTAPI
PsReferenceSiloContext(
    _In_ PVOID SiloContext)
{
    PPS_SILO_CONTEXT_HEADER Header = ((PPS_SILO_CONTEXT_HEADER)SiloContext) - 1;

    InterlockedIncrement(&Header->RefCount);
}

VOID
NTAPI
PsDereferenceSiloContext(
    _In_ PVOID SiloContext)
{
    PPS_SILO_CONTEXT_HEADER Header = ((PPS_SILO_CONTEXT_HEADER)SiloContext) - 1;

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
    PVOID *Table;
    PULONG SlotCount;
    ULONG *PermanentMap;
    NTSTATUS Status;

    PAGED_CODE();

    if ((SiloContext == NULL) || (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX))
        return STATUS_INVALID_PARAMETER;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);

    Table = PsSiloContextStorage(Silo, &SlotCount, &PermanentMap);
    if (ContextSlot >= *SlotCount)
    {
        Status = PsSiloContextGrowTable(Silo, &Table, &SlotCount, ContextSlot + 1);
        if (!NT_SUCCESS(Status))
        {
            ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
            return Status;
        }
    }

    if (Table[ContextSlot] != NULL)
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        return STATUS_NOT_SUPPORTED;
    }

    Table[ContextSlot] = SiloContext;
    InterlockedIncrement(&((PPS_SILO_CONTEXT_HEADER)SiloContext - 1)->RefCount);

    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsGetSiloContext(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot,
    _Out_ PVOID *ReturnedSiloContext)
{
    PVOID *Table;
    PULONG SlotCount;
    ULONG *PermanentMap;
    PVOID Context;

    PAGED_CODE();

    if ((ReturnedSiloContext == NULL) ||
        (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);

    Table = PsSiloContextStorage(Silo, &SlotCount, &PermanentMap);
    if ((ContextSlot >= *SlotCount) || ((Context = Table[ContextSlot]) == NULL))
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_FOUND;
    }

    InterlockedIncrement(&((PPS_SILO_CONTEXT_HEADER)Context - 1)->RefCount);
    *ReturnedSiloContext = Context;

    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PsMakeSiloContextPermanent(
    _In_ PESILO Silo,
    _In_ ULONG ContextSlot)
{
    PVOID *Table;
    PULONG SlotCount;
    ULONG *PermanentMap;

    PAGED_CODE();

    if (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);

    Table = PsSiloContextStorage(Silo, &SlotCount, &PermanentMap);
    if ((ContextSlot >= *SlotCount) || (Table[ContextSlot] == NULL))
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        return STATUS_NOT_FOUND;
    }

    PermanentMap[ContextSlot / 32] |= (1UL << (ContextSlot % 32));

    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
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
    PVOID *Table;
    PULONG SlotCount;
    ULONG *PermanentMap;
    PVOID Context;

    PAGED_CODE();

    if ((ReturnedSiloContext == NULL) ||
        (ContextSlot >= PS_SILO_CONTEXT_SLOTS_MAX) ||
        !PsSiloContextSlotAllocated(ContextSlot))
        return STATUS_INVALID_PARAMETER;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);

    Table = PsSiloContextStorage(Silo, &SlotCount, &PermanentMap);
    if ((ContextSlot >= *SlotCount) || ((Context = Table[ContextSlot]) == NULL))
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_FOUND;
    }

    if (((PermanentMap[ContextSlot / 32] >> (ContextSlot % 32)) & 1) == 0)
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        *ReturnedSiloContext = NULL;
        return STATUS_NOT_SUPPORTED;
    }

    *ReturnedSiloContext = Context;

    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
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
        (Registration->ComponentName->Length == 0) ||
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

    Monitor->ComponentName = *Registration->ComponentName;
    Monitor->ComponentName.Buffer = ExAllocatePoolZero(PagedPool,
                                                       Registration->ComponentName->MaximumLength,
                                                       TAG_SILO_NAME);
    if (Monitor->ComponentName.Buffer == NULL)
    {
        ExFreePoolWithTag(Monitor, TAG_SILO_MONITOR);
        PsFreeSiloContextSlot(ContextSlot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Monitor->ComponentName.Buffer,
                  Registration->ComponentName->Buffer,
                  Registration->ComponentName->MaximumLength);

    Monitor->CreateCallback = Registration->CreateCallback;
    Monitor->TerminateCallback = Registration->TerminateCallback;
    Monitor->MonitorHost = Registration->MonitorHost;
    Monitor->ContextSlot = ContextSlot;

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);
    InsertTailList(&PsSiloMonitorList, &Monitor->Link);
    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);

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

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);
    if (SiloMonitor->Started)
    {
        ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);
        return STATUS_SUCCESS;
    }
    SiloMonitor->Started = TRUE;
    NotifyHost = SiloMonitor->MonitorHost && (SiloMonitor->CreateCallback != NULL);
    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);

    if (NotifyHost)
        Status = SiloMonitor->CreateCallback(NULL);

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

    ExEnterCriticalRegionAndAcquireFastMutex(&PsSiloContextLock);
    if (SiloMonitor->Started && SiloMonitor->MonitorHost &&
        (SiloMonitor->TerminateCallback != NULL))
        NotifyHost = TRUE;
    else
        NotifyHost = FALSE;
    RemoveEntryList(&SiloMonitor->Link);
    ExReleaseFastMutexAndLeaveCriticalRegion(&PsSiloContextLock);

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
    _In_opt_ PEJOB Job)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Job);
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
