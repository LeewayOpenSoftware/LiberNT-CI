/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/wmi/wmi.c
 * PURPOSE:         I/O Windows Management Instrumentation (WMI) Support
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <ntwmi.h>
#define INITGUID
#include <wmiguid.h>
#include <wmidata.h>
#include <wmistr.h>

#include "wmip.h"

#define NDEBUG
#include <debug.h>

typedef struct _WMIP_EVENT_DELIVERY
{
    WORK_QUEUE_ITEM WorkItem;
    PWNODE_HEADER Wnode;
} WMIP_EVENT_DELIVERY, *PWMIP_EVENT_DELIVERY;

typedef struct _WMIP_EVENT_TARGET
{
    PWMIP_GUID_OBJECT GuidObject;
    WMI_NOTIFICATION_CALLBACK Callback;
    PVOID Context;
} WMIP_EVENT_TARGET, *PWMIP_EVENT_TARGET;

EX_PUSH_LOCK WmipNotificationLock;
LIST_ENTRY WmipNotificationList = { &WmipNotificationList, &WmipNotificationList };

typedef enum _WMI_CLOCK_TYPE
{
    WMICT_DEFAULT,
    WMICT_SYSTEMTIME,
    WMICT_PERFCOUNTER,
    WMICT_PROCESS,
    WMICT_THREAD,
    WMICT_CPUCYCLE
} WMI_CLOCK_TYPE;

/* FUNCTIONS *****************************************************************/

BOOLEAN
NTAPI
WmiInitialize(
    VOID)
{
    UNICODE_STRING DriverName = RTL_CONSTANT_STRING(L"\\Driver\\WMIxWDM");
    NTSTATUS Status;

    /* Capture the firmware SMBIOS entry point while the loader block lives */
    WmipCaptureSMBiosFromLoader(KeLoaderBlock);

    WmipInitializeProviders();

    /* Initialize the GUID object type */
    Status = WmipInitializeGuidObjectType();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipInitializeGuidObjectType() failed: 0x%lx\n", Status);
        return FALSE;
    }

    Status = EtwpInitializeRegistrations();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("EtwpInitializeRegistrations() failed: 0x%lx\n", Status);
        return FALSE;
    }

    /* Create the WMI driver */
    Status = IoCreateDriver(&DriverName, WmipDriverEntry);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create WMI driver: 0x%lx\n", Status);
        return FALSE;
    }

    return TRUE;
}

NTSTATUS
NTAPI
IoWMIAllocateInstanceIds(IN GUID *Guid,
                         IN ULONG InstanceCount,
                         OUT ULONG *FirstInstanceId)
{
    if (!Guid || !FirstInstanceId)
        return STATUS_INVALID_PARAMETER;

    return WmipAllocateInstanceIds(Guid, InstanceCount, FirstInstanceId);
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISuggestInstanceName(IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
                         IN PUNICODE_STRING SymbolicLinkName OPTIONAL,
                         IN BOOLEAN CombineNames,
                         OUT PUNICODE_STRING SuggestedInstanceName)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
WmipDeliverEvent(
    _In_ PVOID Context)
{
    PWMIP_EVENT_DELIVERY Delivery = Context;
    PWMIP_GUID_OBJECT GuidObject;
    PWMIP_EVENT_TARGET Targets = NULL;
    PLIST_ENTRY Entry;
    ULONG Count = 0, Index;

    Delivery->Wnode = WmipNameEventInstance(Delivery->Wnode);

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&WmipNotificationLock);
    for (Entry = WmipNotificationList.Flink; Entry != &WmipNotificationList; Entry = Entry->Flink)
    {
        GuidObject = CONTAINING_RECORD(Entry, WMIP_GUID_OBJECT, NotificationLink);
        if (IsEqualGUID(&GuidObject->Guid, &Delivery->Wnode->Guid))
            Count++;
    }
    if (Count)
        Targets = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Targets), 'eimW');
    Count = 0;
    if (Targets)
    {
        for (Entry = WmipNotificationList.Flink; Entry != &WmipNotificationList; Entry = Entry->Flink)
        {
            GuidObject = CONTAINING_RECORD(Entry, WMIP_GUID_OBJECT, NotificationLink);
            if (IsEqualGUID(&GuidObject->Guid, &Delivery->Wnode->Guid) &&
                ObReferenceObjectSafe(GuidObject))
            {
                Targets[Count].GuidObject = GuidObject;
                Targets[Count].Callback = GuidObject->NotificationCallback;
                Targets[Count].Context = GuidObject->NotificationContext;
                Count++;
            }
        }
    }
    ExReleasePushLockShared(&WmipNotificationLock);
    KeLeaveCriticalRegion();

    for (Index = 0; Index < Count; Index++)
    {
        if (Targets[Index].Callback)
            Targets[Index].Callback(Delivery->Wnode, Targets[Index].Context);
        ObDereferenceObject(Targets[Index].GuidObject);
    }

    if (Targets)
        ExFreePoolWithTag(Targets, 'eimW');
    ExFreePool(Delivery->Wnode);
    ExFreePoolWithTag(Delivery, 'eimW');
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIWriteEvent(_Inout_ PVOID WnodeEventItem)
{
    PWNODE_HEADER Header = WnodeEventItem;
    PWMIP_EVENT_DELIVERY Delivery;

    if(!Header)
    {
        DPRINT1("Got NULL Item!\n");
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT1("IoWMIWriteEvent() called for WnodeEventItem %p (Flags = 0x%08lx), returning success\n",
            WnodeEventItem, Header->Flags);

    /* A traced event goes to the session its handle names and the buffer stays the caller's; the session lookup
     * takes a push lock, so above APC_LEVEL (a DPC) the event is dropped until the loggers take events there */
    if (Header->Flags & WNODE_FLAG_TRACED_GUID)
    {
        if (KeGetCurrentIrql() > APC_LEVEL)
            return STATUS_SUCCESS;

        return EtwpTraceClassicEvent((HANDLE)(ULONG_PTR)Header->HistoricalContext,
                                     ((PEVENT_TRACE_HEADER)Header)->Size,
                                     Header,
                                     KernelMode);
    }

    Delivery = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Delivery), 'eimW');
    if (!Delivery)
        return STATUS_INSUFFICIENT_RESOURCES;

    Delivery->Wnode = Header;
    ExInitializeWorkItem(&Delivery->WorkItem, WmipDeliverEvent, Delivery);
    ExQueueWorkItem(&Delivery->WorkItem, DelayedWorkQueue);
    return STATUS_SUCCESS;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIOpenBlock(
    _In_ LPCGUID DataBlockGuid,
    _In_ ULONG DesiredAccess,
    _Out_ PVOID *DataBlockObject)
{
    HANDLE GuidObjectHandle;
    NTSTATUS Status;

    /* Open the GIOD object */
    Status = WmipOpenGuidObject(DataBlockGuid,
                                DesiredAccess,
                                KernelMode,
                                &GuidObjectHandle,
                                DataBlockObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WmipOpenGuidObject failed: 0x%lx\n", Status);
        return Status;
    }

    ZwClose(GuidObjectHandle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmipQueryObjectAllData(
    _In_ PWMIP_GUID_OBJECT GuidObject,
    _Inout_ ULONG *InOutBufferSize,
    _Out_opt_ PVOID OutBuffer)
{
    if (IsEqualGUID(&GuidObject->Guid, &MSSmBios_RawSMBiosTables_GUID))
        return WmipQueryRawSMBiosTables(InOutBufferSize, OutBuffer);

    return WmipQueryAllData(&GuidObject->Guid, InOutBufferSize, OutBuffer);
}

static
NTSTATUS
WmipChainResults(
    _In_ ULONG Count,
    _In_reads_(Count) PVOID *Results,
    _In_reads_(Count) PULONG Sizes,
    _Inout_ ULONG *InOutBufferSize,
    _Out_opt_ PVOID OutBuffer)
{
    PWNODE_HEADER Header;
    ULONG Index, Total = 0, Offset = 0, Last = MAXULONG, Node;

    for (Index = 0; Index < Count; Index++)
    {
        if (Results[Index])
            Total = ALIGN_UP_BY(Total, sizeof(ULONGLONG)) + Sizes[Index];
    }

    if (!Total)
        return STATUS_WMI_GUID_NOT_FOUND;

    if (!OutBuffer || *InOutBufferSize < Total)
    {
        *InOutBufferSize = Total;
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (Index = 0; Index < Count; Index++)
    {
        if (!Results[Index])
            continue;

        Offset = ALIGN_UP_BY(Offset, sizeof(ULONGLONG));
        if (Last != MAXULONG)
            ((PWNODE_HEADER)((PUCHAR)OutBuffer + Last))->Linkage = Offset - Last;
        RtlCopyMemory((PUCHAR)OutBuffer + Offset, Results[Index], Sizes[Index]);

        Node = Offset;
        for (;;)
        {
            Header = (PWNODE_HEADER)((PUCHAR)OutBuffer + Node);
            if (!Header->Linkage || Header->Linkage > Offset + Sizes[Index] - Node)
            {
                Header->Linkage = 0;
                break;
            }
            Node += Header->Linkage;
        }

        Last = Node;
        Offset += Sizes[Index];
    }

    *InOutBufferSize = Total;
    return STATUS_SUCCESS;
}

static
NTSTATUS
WmipQueryMultiple(
    _In_ PVOID *DataBlockObjectList,
    _In_opt_ PUNICODE_STRING InstanceNames,
    _In_ ULONG ObjectCount,
    _Inout_ ULONG *InOutBufferSize,
    _Out_opt_ PVOID OutBuffer)
{
    PWMIP_GUID_OBJECT GuidObject;
    PVOID *Results;
    PULONG Sizes;
    ULONG Index, Size;
    NTSTATUS Status, LastError = STATUS_WMI_GUID_NOT_FOUND;

    if (!DataBlockObjectList || !ObjectCount || !InOutBufferSize)
        return STATUS_INVALID_PARAMETER;

    Results = ExAllocatePoolZero(PagedPool, ObjectCount * (sizeof(PVOID) + sizeof(ULONG)), 'qimW');
    if (!Results)
        return STATUS_INSUFFICIENT_RESOURCES;
    Sizes = (PULONG)&Results[ObjectCount];

    for (Index = 0; Index < ObjectCount; Index++)
    {
        GuidObject = DataBlockObjectList[Index];
        Status = ObReferenceObjectByPointer(GuidObject, WMIGUID_QUERY, WmipGuidObjectType, KernelMode);
        if (!NT_SUCCESS(Status))
        {
            LastError = Status;
            continue;
        }

        Size = 0;
        Status = InstanceNames ?
                 WmipQuerySingleInstance(&GuidObject->Guid, &InstanceNames[Index], &Size, NULL) :
                 WmipQueryObjectAllData(GuidObject, &Size, NULL);
        if ((Status == STATUS_BUFFER_TOO_SMALL || NT_SUCCESS(Status)) && Size)
        {
            Results[Index] = ExAllocatePoolWithTag(NonPagedPoolNx, Size, 'qimW');
            if (!Results[Index])
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else
            {
                Sizes[Index] = Size;
                Status = InstanceNames ?
                         WmipQuerySingleInstance(&GuidObject->Guid, &InstanceNames[Index], &Sizes[Index], Results[Index]) :
                         WmipQueryObjectAllData(GuidObject, &Sizes[Index], Results[Index]);
                if (!NT_SUCCESS(Status))
                {
                    ExFreePoolWithTag(Results[Index], 'qimW');
                    Results[Index] = NULL;
                }
            }
        }

        if (!NT_SUCCESS(Status))
            LastError = Status;

        ObDereferenceObject(GuidObject);
    }

    Status = WmipChainResults(ObjectCount, Results, Sizes, InOutBufferSize, OutBuffer);
    if (Status == STATUS_WMI_GUID_NOT_FOUND)
        Status = LastError;

    for (Index = 0; Index < ObjectCount; Index++)
    {
        if (Results[Index])
            ExFreePoolWithTag(Results[Index], 'qimW');
    }
    ExFreePoolWithTag(Results, 'qimW');
    return Status;
}

BOOLEAN
WmipIsDataBlockRegistered(
    _In_ LPCGUID Guid)
{
    return IsEqualGUID(Guid, &MSSmBios_RawSMBiosTables_GUID) || WmipIsGuidRegistered(Guid);
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQueryAllData(
    IN PVOID DataBlockObject,
    IN OUT ULONG *InOutBufferSize,
    OUT PVOID OutBuffer)
{
    PWMIP_GUID_OBJECT GuidObject;
    NTSTATUS Status;


    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_QUERY,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    GuidObject = DataBlockObject;
    Status = WmipQueryObjectAllData(GuidObject, InOutBufferSize, OutBuffer);

    ObDereferenceObject(DataBlockObject);

    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQueryAllDataMultiple(IN PVOID *DataBlockObjectList,
                          IN ULONG ObjectCount,
                          IN OUT ULONG *InOutBufferSize,
                          OUT PVOID OutBuffer)
{
    return WmipQueryMultiple(DataBlockObjectList, NULL, ObjectCount, InOutBufferSize, OutBuffer);
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstance(IN PVOID DataBlockObject,
                         IN PUNICODE_STRING InstanceName,
                         IN OUT ULONG *InOutBufferSize,
                         OUT PVOID OutBuffer)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    NTSTATUS Status;

    if (!InstanceName || !InOutBufferSize)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_QUERY,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipQuerySingleInstance(&GuidObject->Guid, InstanceName, InOutBufferSize, OutBuffer);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIQuerySingleInstanceMultiple(IN PVOID *DataBlockObjectList,
                                 IN PUNICODE_STRING InstanceNames,
                                 IN ULONG ObjectCount,
                                 IN OUT ULONG *InOutBufferSize,
                                 OUT PVOID OutBuffer)
{
    if (!InstanceNames)
        return STATUS_INVALID_PARAMETER;

    return WmipQueryMultiple(DataBlockObjectList, InstanceNames, ObjectCount, InOutBufferSize, OutBuffer);
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetSingleInstance(IN PVOID DataBlockObject,
                       IN PUNICODE_STRING InstanceName,
                       IN ULONG Version,
                       IN ULONG ValueBufferSize,
                       IN PVOID ValueBuffer)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    NTSTATUS Status;

    if (!InstanceName || (ValueBufferSize && !ValueBuffer))
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_SET,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipSetInstance(&GuidObject->Guid, InstanceName, FALSE, 0, Version, ValueBufferSize, ValueBuffer);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetSingleItem(IN PVOID DataBlockObject,
                   IN PUNICODE_STRING InstanceName,
                   IN ULONG DataItemId,
                   IN ULONG Version,
                   IN ULONG ValueBufferSize,
                   IN PVOID ValueBuffer)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    NTSTATUS Status;

    if (!InstanceName || (ValueBufferSize && !ValueBuffer))
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_SET,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipSetInstance(&GuidObject->Guid, InstanceName, TRUE, DataItemId, Version, ValueBufferSize, ValueBuffer);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIExecuteMethod(IN PVOID DataBlockObject,
                   IN PUNICODE_STRING InstanceName,
                   IN ULONG MethodId,
                   IN ULONG InBufferSize,
                   IN OUT PULONG OutBufferSize,
                   IN OUT PUCHAR InOutBuffer)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    NTSTATUS Status;

    if (!InstanceName || !OutBufferSize || ((InBufferSize || *OutBufferSize) && !InOutBuffer))
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject,
                                        WMIGUID_EXECUTE,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipExecuteMethod(&GuidObject->Guid, InstanceName, MethodId,
                               InBufferSize, OutBufferSize, InOutBuffer);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMISetNotificationCallback(IN PVOID Object,
                             IN WMI_NOTIFICATION_CALLBACK Callback,
                             IN PVOID Context)
{
    PWMIP_GUID_OBJECT GuidObject = Object;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ObReferenceObjectByPointer(Object,
                                        WMIGUID_NOTIFICATION,
                                        WmipGuidObjectType,
                                        KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&WmipNotificationLock);
    GuidObject->NotificationCallback = Callback;
    GuidObject->NotificationContext = Context;
    if (Callback && IsListEmpty(&GuidObject->NotificationLink))
    {
        InsertTailList(&WmipNotificationList, &GuidObject->NotificationLink);
    }
    else if (!Callback && !IsListEmpty(&GuidObject->NotificationLink))
    {
        RemoveEntryList(&GuidObject->NotificationLink);
        InitializeListHead(&GuidObject->NotificationLink);
    }
    ExReleasePushLockExclusive(&WmipNotificationLock);
    KeLeaveCriticalRegion();

    WmipSetEventConsumer(GuidObject, Callback != NULL);

    ObDereferenceObject(Object);
    return STATUS_SUCCESS;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIHandleToInstanceName(IN PVOID DataBlockObject,
                          IN HANDLE FileHandle,
                          OUT PUNICODE_STRING InstanceName)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;

    if (!InstanceName)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject, 0, WmipGuidObjectType, KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(FileHandle, 0, IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    if (NT_SUCCESS(Status))
    {
        Status = WmipDeviceObjectToInstanceName(&GuidObject->Guid,
                                                IoGetRelatedDeviceObject(FileObject),
                                                InstanceName);
        ObDereferenceObject(FileObject);
    }

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
IoWMIDeviceObjectToInstanceName(IN PVOID DataBlockObject,
                                IN PDEVICE_OBJECT DeviceObject,
                                OUT PUNICODE_STRING InstanceName)
{
    PWMIP_GUID_OBJECT GuidObject = DataBlockObject;
    NTSTATUS Status;

    if (!DeviceObject || !InstanceName)
        return STATUS_INVALID_PARAMETER;

    Status = ObReferenceObjectByPointer(DataBlockObject, 0, WmipGuidObjectType, KernelMode);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WmipDeviceObjectToInstanceName(&GuidObject->Guid, DeviceObject, InstanceName);

    ObDereferenceObject(DataBlockObject);
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
WmiQueryTraceInformation(IN TRACE_INFORMATION_CLASS TraceInformationClass,
                         OUT PVOID TraceInformation,
                         IN ULONG TraceInformationLength,
                         OUT PULONG RequiredLength OPTIONAL,
                         IN PVOID Buffer OPTIONAL)
{
    static const UNICODE_STRING GlobalLoggerName = RTL_CONSTANT_STRING(GLOBAL_LOGGER_NAMEW);
    PETW_TRACE_SESSION_SETTINGS Settings;
    WMI_LOGGER_INFORMATION Info;
    ULONG LoggerIds[MAXLOGGERS];
    PUNICODE_STRING Name;
    PTRACEHANDLE Handles;
    ULONG Needed, LoggerId, Logged, Count, i;
    NTSTATUS Status;

    PAGED_CODE();

    if (TraceInformation == NULL) return STATUS_INVALID_PARAMETER;

    switch (TraceInformationClass)
    {
        case TraceIdClass:
        case TraceEnableFlagsClass:
        case TraceEnableLevelClass:
        case LoggerEventsLostClass:
        case LoggerEventsLoggedClass:
            Needed = sizeof(ULONG);
            break;

        case TraceHandleClass:
        case GlobalLoggerHandleClass:
        case AllLoggerHandlesClass:
        case TraceHandleByNameClass:
            Needed = sizeof(TRACEHANDLE);
            break;

        case TraceSessionSettingsClass:
            Needed = sizeof(ETW_TRACE_SESSION_SETTINGS);
            break;

        default:
            /* TODO: the event logger handle and the notify routines the system keeps for itself */
            return STATUS_INVALID_INFO_CLASS;
    }

    if (RequiredLength != NULL) *RequiredLength = Needed;
    if (TraceInformationLength < Needed) return STATUS_INFO_LENGTH_MISMATCH;

    switch (TraceInformationClass)
    {
        case TraceIdClass:
        case TraceEnableFlagsClass:
        case TraceEnableLevelClass:
            if (Buffer == NULL) return STATUS_INVALID_PARAMETER_MIX;

            LoggerId = EtwpLoggerIdFromTraceHandle(((PWNODE_HEADER)Buffer)->HistoricalContext);
            if (!NT_SUCCESS(EtwpQueryLoggerById(LoggerId, NULL, NULL))) return STATUS_INVALID_HANDLE;

            if (TraceInformationClass == TraceIdClass)
            {
                *(PULONG)TraceInformation = LoggerId;
            }
            else
            {
                /* TODO: the enable flags and level a provider was turned on with */
                *(PULONG)TraceInformation = 0;
            }
            return STATUS_SUCCESS;

        case TraceHandleClass:
            if (Buffer == NULL) return STATUS_INVALID_PARAMETER_MIX;

            LoggerId = *(PULONG)Buffer;
            if (!NT_SUCCESS(EtwpQueryLoggerById(LoggerId, NULL, NULL))) return STATUS_INVALID_HANDLE;

            *(PTRACEHANDLE)TraceInformation = EtwpTraceHandleFromLoggerId(LoggerId);
            return STATUS_SUCCESS;

        case GlobalLoggerHandleClass:
            if (!NT_SUCCESS(EtwpFindLoggerByName(&GlobalLoggerName, &LoggerId))) return STATUS_NOT_FOUND;

            *(PTRACEHANDLE)TraceInformation = EtwpTraceHandleFromLoggerId(LoggerId);
            return STATUS_SUCCESS;

        case AllLoggerHandlesClass:
            Count = EtwpEnumerateLoggers(LoggerIds, RTL_NUMBER_OF(LoggerIds));
            if (RequiredLength != NULL) *RequiredLength = Count * sizeof(TRACEHANDLE);

            Handles = TraceInformation;
            for (i = 0; i < Count && i < TraceInformationLength / sizeof(TRACEHANDLE); i++)
            {
                Handles[i] = EtwpTraceHandleFromLoggerId(LoggerIds[i]);
            }
            return (i < Count) ? STATUS_MORE_ENTRIES : STATUS_SUCCESS;

        case TraceHandleByNameClass:
            if (Buffer == NULL) return STATUS_INVALID_PARAMETER_MIX;

            Name = Buffer;
            if (Name->Length == 0 || Name->Buffer == NULL) return STATUS_INVALID_PARAMETER;
            if (!NT_SUCCESS(EtwpFindLoggerByName(Name, &LoggerId))) return STATUS_INVALID_PARAMETER;

            *(PTRACEHANDLE)TraceInformation = EtwpTraceHandleFromLoggerId(LoggerId);
            return STATUS_SUCCESS;

        case LoggerEventsLostClass:
        case LoggerEventsLoggedClass:
            if (Buffer == NULL) return STATUS_INVALID_PARAMETER_MIX;

            Status = EtwpQueryLoggerById(*(PULONG)Buffer, &Info, &Logged);
            if (!NT_SUCCESS(Status)) return STATUS_INVALID_HANDLE;

            *(PULONG)TraceInformation = (TraceInformationClass == LoggerEventsLostClass) ? Info.EventsLost : Logged;
            return STATUS_SUCCESS;

        case TraceSessionSettingsClass:
            if (Buffer == NULL) return STATUS_INVALID_PARAMETER_MIX;

            LoggerId = EtwpLoggerIdFromTraceHandle(*(PTRACEHANDLE)Buffer);
            Status = EtwpQueryLoggerById(LoggerId, &Info, NULL);
            if (!NT_SUCCESS(Status)) return STATUS_INVALID_HANDLE;

            Settings = TraceInformation;
            Settings->Version = 1;
            Settings->BufferSize = Info.BufferSize;
            Settings->MinimumBuffers = Info.MinimumBuffers;
            Settings->MaximumBuffers = Info.MaximumBuffers;
            Settings->LoggerMode = Info.LogFileMode;
            Settings->FlushTimer = Info.FlushTimer;
            Settings->FlushThreshold = (ULONG)Info.FlushThreshold;
            Settings->ClockType = Info.Wnode.ClientContext;
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_INFO_CLASS;
    }
}

/*
 * @implemented
 *
 * Logs a WPP trace message to the session named by LoggerHandle: the message number leads the record and
 * the caller's (pointer, length) argument pairs follow, ending at a NULL pointer. The event goes through
 * the classic-event path, so it counts against the session like any other event (EventsLogged).
 */
NTSTATUS
NTAPI
WmiTraceMessageVa(IN TRACEHANDLE LoggerHandle,
                  IN ULONG MessageFlags,
                  IN LPGUID MessageGuid,
                  IN USHORT MessageNumber,
                  IN va_list MessageArgList)
{
    PUCHAR Record, Data, End;
    PEVENT_TRACE_HEADER Header;
    PVOID Source;
    SIZE_T Length;
    NTSTATUS Status;

    /* WPP drivers log from DPCs; the logger lookup takes a push lock and the buffer a guarded mutex, both
     * only safe at or below APC_LEVEL, so above it drop the message silently as IoWMIWriteEvent does. */
    if (KeGetCurrentIrql() > APC_LEVEL)
        return STATUS_SUCCESS;

    /* Every WPP driver calls this, most often with no session running; reject a handle that names none
     * (zero, or out of the logger-id range) before allocating and copying the message. */
    if (!LoggerHandle || EtwpLoggerIdFromTraceHandle(LoggerHandle) >= MAXLOGGERS)
        return STATUS_INVALID_HANDLE;

    /* Header, the leading message number, and the message data (WPP caps one message at 8 KB). */
    Record = ExAllocatePoolWithTag(NonPagedPoolNx,
                                   sizeof(EVENT_TRACE_HEADER) + sizeof(USHORT) + TRACE_MESSAGE_MAXIMUM_SIZE,
                                   'mTwE');
    if (!Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header = (PEVENT_TRACE_HEADER)Record;
    RtlZeroMemory(Header, sizeof(*Header));
    Header->Flags = WNODE_FLAG_TRACED_GUID;
    if ((MessageFlags & TRACE_MESSAGE_GUID) && MessageGuid)
        Header->Guid = *MessageGuid;

    Data = Record + sizeof(EVENT_TRACE_HEADER);
    End = Record + sizeof(EVENT_TRACE_HEADER) + sizeof(USHORT) + TRACE_MESSAGE_MAXIMUM_SIZE;

    *(PUSHORT)Data = MessageNumber;
    Data += sizeof(USHORT);

    for (;;)
    {
        Source = va_arg(MessageArgList, PVOID);
        if (!Source)
            break;
        /* Fetch the SIZE_T slot (correct on both targets) but keep only the low 32 bits: a caller that
         * passes a ULONG leaves the upper half of the amd64 slot undefined. */
        Length = (ULONG)va_arg(MessageArgList, SIZE_T);
        if (Length == 0)
            continue;
        if (Length > (SIZE_T)(End - Data))
        {
            ExFreePoolWithTag(Record, 'mTwE');
            return STATUS_BUFFER_OVERFLOW;
        }
        RtlCopyMemory(Data, Source, Length);
        Data += Length;
    }

    Header->Size = (USHORT)(Data - Record);
    Status = EtwpTraceClassicEvent((HANDLE)(ULONG_PTR)LoggerHandle,
                                   (ULONG)(Data - Record), Record, KernelMode);
    ExFreePoolWithTag(Record, 'mTwE');
    return Status;
}

/*
 * @implemented
 */
NTSTATUS
__cdecl
WmiTraceMessage(IN TRACEHANDLE LoggerHandle,
                IN ULONG MessageFlags,
                IN LPGUID MessageGuid,
                IN USHORT MessageNumber,
                IN ...)
{
    va_list MessageArgList;
    NTSTATUS Status;

    /* The published ABI uses USHORT as the final named parameter. Clang warns about its default
     * promotion even though the Windows ABI va-list builtin handles the promoted slot (see
     * ntoskrnl/rtl/misc.c for the same case). */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvarargs"
#endif
    va_start(MessageArgList, MessageNumber);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    Status = WmiTraceMessageVa(LoggerHandle, MessageFlags, MessageGuid, MessageNumber, MessageArgList);
    va_end(MessageArgList);
    return Status;
}

NTSTATUS
NTAPI
WmiFlushTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

LONG64
FASTCALL
WmiGetClock(IN WMI_CLOCK_TYPE ClockType,
            IN PVOID Context)
{
    LARGE_INTEGER Time;

    UNREFERENCED_PARAMETER(Context);

    /* The trace clocks: system time, the processor's own cycle counter, else the performance counter */
    switch (ClockType)
    {
        case EVENT_TRACE_CLOCK_SYSTEMTIME:
            KeQuerySystemTime(&Time);
            return Time.QuadPart;

        case EVENT_TRACE_CLOCK_CPUCYCLE:
            return (LONGLONG)ReadTimeStampCounter();

        default:
            return KeQueryPerformanceCounter(NULL).QuadPart;
    }
}

NTSTATUS
NTAPI
WmiQueryTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiStartTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    return EtwpTraceControlKernel(EtwStartLoggerCode, LoggerInfo);
}

NTSTATUS
NTAPI
WmiStopTrace(IN PWMI_LOGGER_INFORMATION LoggerInfo)
{
    return EtwpTraceControlKernel(EtwStopLoggerCode, LoggerInfo);
}

NTSTATUS
FASTCALL
WmiTraceFastEvent(IN PWNODE_HEADER Wnode)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
WmiUpdateTrace(IN OUT PWMI_LOGGER_INFORMATION LoggerInfo)
{
    return EtwpTraceControlKernel(EtwUpdateLoggerCode, LoggerInfo);
}

ULONG
NTAPI
EtwpDisableStackWalkApc(VOID)
{
    PKTHREAD Thread = KeGetCurrentThread();
    LONG OldFlags;
    LONG NewFlags;
    LONG ActualFlags;

    OldFlags = ReadAcquire(&Thread->ThreadFlags);
    for (;;)
    {
        NewFlags = OldFlags | (LONG)0xFF800000;
        ActualFlags = InterlockedCompareExchange(&Thread->ThreadFlags,
                                                 NewFlags,
                                                 OldFlags);
        if (ActualFlags == OldFlags)
            return ((ULONG)OldFlags >> 23);

        OldFlags = ActualFlags;
    }
}

VOID
NTAPI
EtwpReenableStackWalkApc(
    _In_ ULONG PreviousState)
{
    PKTHREAD Thread = KeGetCurrentThread();
    LONG OldFlags;
    LONG NewFlags;
    LONG ActualFlags;
    ULONG RestoreMask;

    RestoreMask = (PreviousState << 23) | 0x007FFFFF;
    OldFlags = ReadAcquire(&Thread->ThreadFlags);
    for (;;)
    {
        NewFlags = OldFlags & (LONG)RestoreMask;
        ActualFlags = InterlockedCompareExchange(&Thread->ThreadFlags,
                                                 NewFlags,
                                                 OldFlags);
        if (ActualFlags == OldFlags)
            return;

        OldFlags = ActualFlags;
    }
}

/*Eof*/
