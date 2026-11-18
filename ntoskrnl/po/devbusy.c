/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device idle detection and device busy references
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

typedef struct _POP_IDLE_RECORD
{
    LIST_ENTRY Link;
    PDEVICE_OBJECT DeviceObject;
    volatile LONG References;
    volatile LONG BusyCount;
    ULONG ConservationIdleTime;
    ULONG PerformanceIdleTime;
    DEVICE_POWER_STATE State;
    BOOLEAN Registered;
    BOOLEAN IdleSent;
    BOOLEAN Queued;
    LIST_ENTRY WorkLink;
    volatile ULONG IdleCount;
} POP_IDLE_RECORD, *PPOP_IDLE_RECORD;

static KSPIN_LOCK PopIdleLock;
static LIST_ENTRY PopIdleList = { &PopIdleList, &PopIdleList };
static LIST_ENTRY PopIdleWorkList = { &PopIdleWorkList, &PopIdleWorkList };
static KTIMER PopIdleTimer;
static KDPC PopIdleDpc;
static WORK_QUEUE_ITEM PopIdleWorkItem;
static BOOLEAN PopIdleWorkQueued;
static volatile LONG PopIdleInitialized;
static PVOID PopIdlePowerSourceHandle;
static volatile LONG PopIdleOnDc;

static
VOID
PopIdleDereference(
    _In_ PPOP_IDLE_RECORD Record)
{
    if (InterlockedDecrement(&Record->References) == 0)
    {
        ObDereferenceObject(Record->DeviceObject);
        ExFreePoolWithTag(Record, TAG_PO_DOPE);
    }
}

static
NTSTATUS
NTAPI
PopIdlePowerSourceCallback(
    _In_ LPCGUID SettingGuid,
    _In_ PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(SettingGuid);
    UNREFERENCED_PARAMETER(Context);

    if (Value && ValueLength >= sizeof(ULONG))
        InterlockedExchange(&PopIdleOnDc, *(PULONG)Value != PoAc);

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
PopIdlePowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ UCHAR MinorFunction,
    _In_ POWER_STATE PowerState,
    _In_opt_ PVOID Context,
    _In_ PIO_STATUS_BLOCK IoStatus)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);
    UNREFERENCED_PARAMETER(IoStatus);

    PopIdleDereference((PPOP_IDLE_RECORD)Context);
}

static
VOID
NTAPI
PopIdleWorker(
    _In_ PVOID Parameter)
{
    PPOP_IDLE_RECORD Record;
    PLIST_ENTRY Entry;
    POWER_STATE PowerState;
    KIRQL OldIrql;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Parameter);

    KeAcquireSpinLock(&PopIdleLock, &OldIrql);
    while (!IsListEmpty(&PopIdleWorkList))
    {
        Entry = RemoveHeadList(&PopIdleWorkList);
        Record = CONTAINING_RECORD(Entry, POP_IDLE_RECORD, WorkLink);
        Record->Queued = FALSE;
        PowerState.DeviceState = Record->State;
        KeReleaseSpinLock(&PopIdleLock, OldIrql);

        /* The queue entry's reference moves to the IRP, or is dropped when there is none */
        if (Record->Registered)
        {
            Status = PoRequestPowerIrp(Record->DeviceObject, IRP_MN_SET_POWER, PowerState,
                                       PopIdlePowerComplete, Record, NULL);
            if (!NT_SUCCESS(Status))
                PopIdleDereference(Record);
        }
        else
        {
            PopIdleDereference(Record);
        }

        KeAcquireSpinLock(&PopIdleLock, &OldIrql);
    }
    PopIdleWorkQueued = FALSE;
    KeReleaseSpinLock(&PopIdleLock, OldIrql);
}

static
VOID
NTAPI
PopIdleTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_IDLE_RECORD Record;
    PLIST_ENTRY Entry;
    ULONG Limit, Count;
    BOOLEAN OnDc = (ReadNoFence(&PopIdleOnDc) != 0);

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&PopIdleLock);
    for (Entry = PopIdleList.Flink; Entry != &PopIdleList; Entry = Entry->Flink)
    {
        Record = CONTAINING_RECORD(Entry, POP_IDLE_RECORD, Link);
        Limit = OnDc ? Record->ConservationIdleTime : Record->PerformanceIdleTime;

        /* A busy reference holds the device busy for as long as it is held */
        if (ReadNoFence(&Record->BusyCount) > 0)
        {
            InterlockedExchange((PLONG)&Record->IdleCount, 0);
            Record->IdleSent = FALSE;
            continue;
        }

        Count = (ULONG)InterlockedIncrement((PLONG)&Record->IdleCount);
        if (Count < Limit || Limit == 0)
        {
            Record->IdleSent = FALSE;
            continue;
        }

        if (Record->IdleSent || Record->Queued)
            continue;

        Record->IdleSent = TRUE;
        Record->Queued = TRUE;
        InterlockedIncrement(&Record->References);
        InsertTailList(&PopIdleWorkList, &Record->WorkLink);
        if (!PopIdleWorkQueued)
        {
            PopIdleWorkQueued = TRUE;
            ExQueueWorkItem(&PopIdleWorkItem, DelayedWorkQueue);
        }
    }
    KeReleaseSpinLockFromDpcLevel(&PopIdleLock);
}

static
VOID
PopIdleInitialize(VOID)
{
    LARGE_INTEGER DueTime;

    if (InterlockedCompareExchange(&PopIdleInitialized, 1, 0) != 0)
        return;

    KeInitializeSpinLock(&PopIdleLock);
    ExInitializeWorkItem(&PopIdleWorkItem, PopIdleWorker, NULL);
    KeInitializeDpc(&PopIdleDpc, PopIdleTimerDpc, NULL);
    KeInitializeTimerEx(&PopIdleTimer, NotificationTimer);

    /* The callback reports the current power source when it is registered */
    PoRegisterPowerSettingCallback(NULL, &GUID_ACDC_POWER_SOURCE, PopIdlePowerSourceCallback,
                                   NULL, &PopIdlePowerSourceHandle);

    DueTime.QuadPart = -10000000LL;
    KeSetTimerEx(&PopIdleTimer, DueTime, 1000, &PopIdleDpc);
    InterlockedExchange(&PopIdleInitialized, 2);
}

static
PPOP_IDLE_RECORD
PopIdleFindRecordLocked(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PLIST_ENTRY Entry;
    PPOP_IDLE_RECORD Record;

    for (Entry = PopIdleList.Flink; Entry != &PopIdleList; Entry = Entry->Flink)
    {
        Record = CONTAINING_RECORD(Entry, POP_IDLE_RECORD, Link);
        if (Record->DeviceObject == DeviceObject)
            return Record;
    }
    return NULL;
}

PULONG
NTAPI
PoRegisterDeviceForIdleDetection(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ConservationIdleTime,
    _In_ ULONG PerformanceIdleTime,
    _In_ DEVICE_POWER_STATE State)
{
    PPOP_IDLE_RECORD Record, NewRecord = NULL;
    PULONG IdlePointer = NULL;
    KIRQL OldIrql;

    PAGED_CODE();

    if (!DeviceObject)
        return NULL;

    /* Zero times cancel the registration */
    if (ConservationIdleTime == 0 && PerformanceIdleTime == 0)
    {
        if (ReadNoFence(&PopIdleInitialized) != 2)
            return NULL;

        KeAcquireSpinLock(&PopIdleLock, &OldIrql);
        Record = PopIdleFindRecordLocked(DeviceObject);
        if (Record)
        {
            RemoveEntryList(&Record->Link);
            Record->Registered = FALSE;
        }
        KeReleaseSpinLock(&PopIdleLock, OldIrql);

        if (Record)
            PopIdleDereference(Record);
        return NULL;
    }

    if (State <= PowerDeviceD0 || State >= PowerDeviceMaximum)
        return NULL;

    PopIdleInitialize();
    while (ReadNoFence(&PopIdleInitialized) != 2)
        YieldProcessor();

    NewRecord = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*NewRecord), TAG_PO_DOPE);
    if (!NewRecord)
        return NULL;

    KeAcquireSpinLock(&PopIdleLock, &OldIrql);
    Record = PopIdleFindRecordLocked(DeviceObject);
    if (!Record)
    {
        Record = NewRecord;
        NewRecord = NULL;
        Record->DeviceObject = DeviceObject;
        Record->References = 1;
        Record->Registered = TRUE;
        ObReferenceObject(DeviceObject);
        InsertTailList(&PopIdleList, &Record->Link);
    }
    Record->ConservationIdleTime = ConservationIdleTime;
    Record->PerformanceIdleTime = PerformanceIdleTime;
    Record->State = State;
    Record->IdleCount = 0;
    Record->IdleSent = FALSE;
    IdlePointer = (PULONG)&Record->IdleCount;
    KeReleaseSpinLock(&PopIdleLock, OldIrql);

    if (NewRecord)
        ExFreePoolWithTag(NewRecord, TAG_PO_DOPE);

    return IdlePointer;
}

VOID
NTAPI
PoSetDeviceBusyEx(
    _Inout_ PULONG IdlePointer)
{
    if (IdlePointer)
        InterlockedExchange((PLONG)IdlePointer, 0);
}

VOID
NTAPI
PoStartDeviceBusy(
    _Inout_ PULONG IdlePointer)
{
    PPOP_IDLE_RECORD Record;

    if (!IdlePointer)
        return;

    Record = CONTAINING_RECORD(IdlePointer, POP_IDLE_RECORD, IdleCount);
    InterlockedIncrement(&Record->BusyCount);
    InterlockedExchange((PLONG)IdlePointer, 0);
}

VOID
NTAPI
PoEndDeviceBusy(
    _Inout_ PULONG IdlePointer)
{
    PPOP_IDLE_RECORD Record;
    LONG Count;

    if (!IdlePointer)
        return;

    Record = CONTAINING_RECORD(IdlePointer, POP_IDLE_RECORD, IdleCount);
    Count = InterlockedDecrement(&Record->BusyCount);
    ASSERT(Count >= 0);
    if (Count < 0)
        InterlockedIncrement(&Record->BusyCount);

    /* The idle period starts when the last busy reference ends */
    InterlockedExchange((PLONG)IdlePointer, 0);
}

