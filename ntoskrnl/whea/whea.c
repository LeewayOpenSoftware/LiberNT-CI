/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hardware error sources and the queue of reported hardware errors
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#include <evntprov.h>
#define NDEBUG
#include <debug.h>

#define WHEAP_TAG 'aehW'
#define WHEAP_MAX_DESCRIPTOR_LENGTH 0x2000
#define WHEAP_MAX_RAW_DATA_LENGTH 0x10000
#define WHEAP_MAX_RECORDS 64
#define WHEAP_MAX_RECORD_BYTES 0x100000
#define WHEAP_RECORD_REVISION 0x0210
#define WHEAP_SECTION_REVISION 0x0300
#define WHEAP_DEBUG_LINES 8
#define WHEAP_DEBUG_LINE_INTERVAL 256
#define WHEAP_REMOVE_WAIT_ATTEMPTS 100
#define WHEAP_REMOVE_WAIT_INTERVAL (-10 * 10000)
#define WHEAP_EVENT_CHANNEL_SYSTEM 8
#define WHEAP_EVENT_KEYWORDS 0x8000000000000002ULL
#define WHEAP_MAX_SECTIONS 32
#define WHEAP_LOG_BUFFER_LENGTH \
    (sizeof(WHEA_ERROR_RECORD_HEADER) + WHEAP_MAX_SECTIONS * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) + \
     sizeof(WHEA_ERROR_PACKET_V2) + WHEAP_MAX_RAW_DATA_LENGTH)

C_ASSERT(sizeof(WHEA_ERROR_RECORD_HEADER) == 128);
C_ASSERT(sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) == 72);
C_ASSERT(sizeof(WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1) == 28 + 2 * sizeof(PVOID));
C_ASSERT(sizeof(WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER) == 68 + 2 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER, Initialize) == 28);
C_ASSERT(sizeof(WHEA_DRIVER_BUFFER_SET) == 8 + 4 * sizeof(PVOID));
C_ASSERT(sizeof(WHEA_ERROR_SOURCE_CONFIGURATION) == 7 * sizeof(PVOID));
C_ASSERT(sizeof(MCG_CAP) == 8 && sizeof(MCG_STATUS) == 8 && sizeof(MCI_STATUS) == 8);
C_ASSERT(sizeof(XPF_RECOVERY_INFO) == 20);
C_ASSERT(FIELD_OFFSET(WHEA_XPF_MCA_SECTION, GlobalCapability) == 264);
C_ASSERT(FIELD_OFFSET(WHEA_XPF_MCA_SECTION, ExBankCount) == 292);
C_ASSERT(sizeof(WHEA_XPF_MCA_SECTION) == 1192);
C_ASSERT(RTL_FIELD_SIZE(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR, FRUText) == WHEA_GENERIC_ENTRY_TEXT_LEN);

typedef struct _WHEAP_DESCRIPTOR_HEADER
{
    ULONG Length;
    ULONG Version;
    WHEA_ERROR_SOURCE_TYPE Type;
    WHEA_ERROR_SOURCE_STATE State;
    ULONG MaxRawDataLength;
    ULONG NumRecordsToPreallocate;
    ULONG MaxSectionsPerRecord;
    ULONG ErrorSourceId;
    ULONG PlatformErrorSourceId;
    ULONG Flags;
} WHEAP_DESCRIPTOR_HEADER, *PWHEAP_DESCRIPTOR_HEADER;

typedef struct _WHEAP_ERROR_SOURCE *PWHEAP_ERROR_SOURCE;

typedef struct _WHEAP_RECORD
{
    LIST_ENTRY Link;
    PWHEAP_ERROR_SOURCE Source;
    LARGE_INTEGER SystemTime;
    ULONG PacketLength;
    BOOLEAN DeviceDriver;
    BOOLEAN FromPool;
    WHEA_ERROR_SEVERITY Severity;
    ULONG SectionSlots;
    ULONG SectionCount;
    ULONG DataCapacity;
    UCHAR Packet[ANYSIZE_ARRAY];
} WHEAP_RECORD, *PWHEAP_RECORD;

typedef struct _WHEAP_ERROR_SOURCE
{
    LIST_ENTRY Link;
    ULONG ErrorSourceId;
    WHEA_ERROR_SOURCE_STATE State;
    PVOID Context;
    ULONG MaxPacketLength;
    ULONG ReportedCount;
    ULONG DroppedCount;
    ULONG LoggedCount;
    BOOLEAN DeviceDriver;
    ULONG MaxSections;
    WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER Uninitialize;
    GUID CreatorId;
    GUID PartitionId;
    LIST_ENTRY FreeRecords;
    ULONG DescriptorLength;
    UCHAR Descriptor[ANYSIZE_ARRAY];
} WHEAP_ERROR_SOURCE;

static KSPIN_LOCK WheapLock;
static LIST_ENTRY WheapSourceList = { &WheapSourceList, &WheapSourceList };
static LIST_ENTRY WheapPendingList = { &WheapPendingList, &WheapPendingList };
static LIST_ENTRY WheapOpenList = { &WheapOpenList, &WheapOpenList };
static LIST_ENTRY WheapDiscardList = { &WheapDiscardList, &WheapDiscardList };
static ULONG WheapNextSourceId = 1;
static KDPC WheapDpc;
static WORK_QUEUE_ITEM WheapWorkItem;
static BOOLEAN WheapConsumerReady;
static BOOLEAN WheapWorkQueued;
static LONG64 WheapLastRecordId;
static LONG64 WheapRecordIdBase;

static PUCHAR WheapLogBuffer;
static REGHANDLE WheapEtwHandle;
static BOOLEAN WheapEtwRegistered;

static const GUID WheapLoggerGuid = { 0xC26C4F3C, 0x3F66, 0x4E99, { 0x8F, 0x8A, 0x39, 0x40, 0x5C, 0xFE, 0xD2, 0x20 } };

static const GUID WheapRecordCreatorGuid = { 0xCF07C4BD, 0xB789, 0x4E18, { 0xB3, 0xC4, 0x1F, 0x73, 0x2C, 0xB5, 0x71, 0x31 } };
static const GUID WheapErrorPacketSectionGuid = { 0xE71254E9, 0xC1B9, 0x4940, { 0xAB, 0x76, 0x90, 0x97, 0x03, 0xA4, 0x32, 0x0F } };

static
VOID
WheapAcquireLock(
    _Out_ PKIRQL OldIrql)
{
    KeRaiseIrql(HIGH_LEVEL, OldIrql);
    KiAcquireSpinLock(&WheapLock);
}

static
VOID
WheapReleaseLock(
    _In_ KIRQL OldIrql)
{
    KiReleaseSpinLock(&WheapLock);
    KeLowerIrql(OldIrql);
}

static
PWHEAP_ERROR_SOURCE
WheapFindSource(
    _In_ ULONG ErrorSourceId)
{
    PLIST_ENTRY Entry;
    PWHEAP_ERROR_SOURCE Source;

    for (Entry = WheapSourceList.Flink; Entry != &WheapSourceList; Entry = Entry->Flink)
    {
        Source = CONTAINING_RECORD(Entry, WHEAP_ERROR_SOURCE, Link);
        if (Source->ErrorSourceId == ErrorSourceId)
            return Source;
    }

    return NULL;
}

static
PUCHAR
WheapRecordData(
    _In_ PWHEAP_RECORD Record)
{
    return Record->Packet + Record->SectionSlots * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR);
}

static
PWHEAP_RECORD
WheapFindOpenReport(
    _In_opt_ WHEA_ERROR_HANDLE ErrorHandle)
{
    PLIST_ENTRY Entry;

    for (Entry = WheapOpenList.Flink; Entry != &WheapOpenList; Entry = Entry->Flink)
    {
        if ((PVOID)CONTAINING_RECORD(Entry, WHEAP_RECORD, Link) == ErrorHandle)
            return CONTAINING_RECORD(Entry, WHEAP_RECORD, Link);
    }

    return NULL;
}

static
VOID
WheapOpenReport(
    _Inout_ PWHEAP_RECORD Record)
{
    Record->DeviceDriver = TRUE;
    /* FIXME: the severity of a report whose driver never sets one is not documented; recoverable is assumed */
    Record->Severity = WheaErrSevRecoverable;
    Record->SectionCount = 0;
    Record->PacketLength = 0;
    InsertTailList(&WheapOpenList, &Record->Link);
}

static
PWHEAP_RECORD
WheapRecycleRecord(
    _In_ PWHEAP_RECORD Record,
    _In_ KIRQL CallerIrql,
    _Inout_ PBOOLEAN QueueDpc)
{
    if (!Record->FromPool)
    {
        InsertTailList(&Record->Source->FreeRecords, &Record->Link);
        return NULL;
    }

    if (CallerIrql <= DISPATCH_LEVEL)
        return Record;

    InsertTailList(&WheapDiscardList, &Record->Link);
    if (!WheapWorkQueued)
    {
        WheapWorkQueued = TRUE;
        *QueueDpc = TRUE;
    }

    return NULL;
}

static
VOID
WheapFreeRecords(
    _Inout_ PLIST_ENTRY Records)
{
    while (!IsListEmpty(Records))
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(Records), WHEAP_RECORD, Link), WHEAP_TAG);
}

static
BOOLEAN
WheapIsRangeInPacket(
    _In_ ULONG Offset,
    _In_ ULONG DataLength,
    _In_ ULONG PacketLength)
{
    if (DataLength == 0)
        return TRUE;

    return Offset >= sizeof(WHEA_ERROR_PACKET_V2) &&
           Offset <= PacketLength &&
           DataLength <= PacketLength - Offset;
}

static
ULONGLONG
WheapNextRecordId(VOID)
{
    LONG64 Base = InterlockedCompareExchange64(&WheapRecordIdBase, 0, 0);
    ULONG BootSeconds;

    if (Base == 0 && RtlTimeToSecondsSince1980(&KeBootTime, &BootSeconds))
    {
        InterlockedCompareExchange64(&WheapRecordIdBase, (LONG64)BootSeconds << 32, 0);
        Base = InterlockedCompareExchange64(&WheapRecordIdBase, 0, 0);
    }

    return (ULONGLONG)Base + (ULONGLONG)InterlockedIncrement64(&WheapLastRecordId);
}

NTSTATUS
NTAPI
WheaInitializeRecordHeader(
    _Out_ PWHEA_ERROR_RECORD_HEADER Header)
{
    LARGE_INTEGER SystemTime;
    TIME_FIELDS Time;

    if (!Header)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Header, sizeof(*Header));
    Header->Signature = WHEA_ERROR_RECORD_SIGNATURE;
    Header->Revision.AsUSHORT = WHEAP_RECORD_REVISION;
    Header->SignatureEnd = WHEA_ERROR_RECORD_SIGNATURE_END;
    Header->Length = sizeof(*Header);
    Header->CreatorId = WheapRecordCreatorGuid;
    Header->RecordId = WheapNextRecordId();

    KeQuerySystemTime(&SystemTime);
    RtlTimeToTimeFields(&SystemTime, &Time);
    Header->Timestamp.Seconds = Time.Second;
    Header->Timestamp.Minutes = Time.Minute;
    Header->Timestamp.Hours = Time.Hour;
    Header->Timestamp.Day = Time.Day;
    Header->Timestamp.Month = Time.Month;
    Header->Timestamp.Year = Time.Year % 100;
    Header->Timestamp.Century = Time.Year / 100;
    Header->ValidBits.Timestamp = 1;
    return STATUS_SUCCESS;
}

static
VOID
WheapWriteRecord(
    _In_ PWHEA_ERROR_RECORD_HEADER Header,
    _In_ WHEA_ERROR_SEVERITY Severity);

static
VOID
WheapLogPacket(
    _In_ ULONG PacketLength)
{
    PWHEA_ERROR_RECORD_HEADER Header = (PWHEA_ERROR_RECORD_HEADER)WheapLogBuffer;
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)(Header + 1);
    PWHEA_ERROR_PACKET_V2 Packet = (PWHEA_ERROR_PACKET_V2)(Section + 1);

    WheaInitializeRecordHeader(Header);
    Header->SectionCount = 1;
    Header->Severity = Packet->ErrorSeverity;
    Header->NotifyType = Packet->NotifyType;
    Header->Flags.PreviousError = Packet->Flags.PreviousError;
    Header->Flags.Simulated = Packet->Flags.Simulated;
    Header->Length = sizeof(*Header) + sizeof(*Section) + PacketLength;

    RtlZeroMemory(Section, sizeof(*Section));
    Section->SectionOffset = sizeof(*Header) + sizeof(*Section);
    Section->SectionLength = PacketLength;
    Section->Revision.AsUSHORT = WHEAP_SECTION_REVISION;
    Section->SectionType = WheapErrorPacketSectionGuid;
    Section->SectionSeverity = Packet->ErrorSeverity;
    WheapWriteRecord(Header, Packet->ErrorSeverity);
}

static
VOID
WheapLogDeviceDriverData(
    _In_ const WHEAP_RECORD *Item,
    _In_ const GUID *CreatorId,
    _In_ const GUID *PartitionId)
{
    static const GUID NoGuid = { 0 };
    PWHEA_ERROR_RECORD_HEADER Header = (PWHEA_ERROR_RECORD_HEADER)WheapLogBuffer;
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)(Header + 1);
    ULONG Offset = sizeof(*Header) + Item->SectionCount * sizeof(*Section);
    ULONG Index;

    WheaInitializeRecordHeader(Header);
    Header->SectionCount = (USHORT)Item->SectionCount;
    Header->Severity = Item->Severity;
    Header->Flags.DeviceDriver = 1;
    Header->Length = Offset + Item->PacketLength;
    if (!RtlEqualMemory(CreatorId, &NoGuid, sizeof(GUID)))
        Header->CreatorId = *CreatorId;
    if (!RtlEqualMemory(PartitionId, &NoGuid, sizeof(GUID)))
    {
        Header->PartitionId = *PartitionId;
        Header->ValidBits.PartitionId = 1;
    }

    for (Index = 0; Index < Item->SectionCount; Index++)
    {
        Section[Index].SectionOffset = Offset;
        Offset += Section[Index].SectionLength;
        Section[Index].Revision.AsUSHORT = WHEAP_SECTION_REVISION;
        Section[Index].ValidBits.FRUText = Section[Index].FRUText[0] != 0;
    }

    WheapWriteRecord(Header, Item->Severity);
}

static
VOID
WheapWriteRecord(
    _In_ PWHEA_ERROR_RECORD_HEADER Header,
    _In_ WHEA_ERROR_SEVERITY Severity)
{
    EVENT_DATA_DESCRIPTOR Data[2];
    EVENT_DESCRIPTOR Event;
    ULONG Length = Header->Length;

    if (!WheapEtwRegistered)
        WheapEtwRegistered = NT_SUCCESS(EtwRegister(&WheapLoggerGuid, NULL, NULL, &WheapEtwHandle));
    if (!WheapEtwRegistered)
        return;

    RtlZeroMemory(&Event, sizeof(Event));
    Event.Channel = WHEAP_EVENT_CHANNEL_SYSTEM;
    Event.Keyword = WHEAP_EVENT_KEYWORDS;
    if (Severity == WheaErrSevCorrected)
    {
        Event.Id = 2;
        Event.Level = 3;
    }
    else if (Severity == WheaErrSevInformational)
    {
        Event.Id = 3;
        Event.Level = 4;
    }
    else
    {
        Event.Id = 1;
        Event.Level = 2;
    }

    EventDataDescCreate(&Data[0], &Length, sizeof(Length));
    EventDataDescCreate(&Data[1], Header, Length);
    EtwWrite(WheapEtwHandle, &Event, NULL, RTL_NUMBER_OF(Data), Data);
}

static
VOID
NTAPI
WheapWorker(
    _In_ PVOID Parameter)
{
    PWHEAP_RECORD Record;
    PWHEA_ERROR_PACKET_V2 Packet;
    WHEAP_RECORD Item;
    GUID CreatorId, PartitionId;
    ULONG ErrorSourceId, ErrorType, Length, Sequence, SectionBytes;
    LIST_ENTRY Discarded;
    BOOLEAN FromPool;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Parameter);

    if (!WheapLogBuffer)
        WheapLogBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, WHEAP_LOG_BUFFER_LENGTH, WHEAP_TAG);

    InitializeListHead(&Discarded);
    for (;;)
    {
        WheapAcquireLock(&OldIrql);
        while (!IsListEmpty(&WheapDiscardList))
            InsertTailList(&Discarded, RemoveHeadList(&WheapDiscardList));

        if (IsListEmpty(&WheapPendingList))
        {
            WheapWorkQueued = FALSE;
            WheapReleaseLock(OldIrql);
            WheapFreeRecords(&Discarded);
            break;
        }

        Record = CONTAINING_RECORD(RemoveHeadList(&WheapPendingList), WHEAP_RECORD, Link);
        Packet = (PWHEA_ERROR_PACKET_V2)Record->Packet;
        RtlCopyMemory(&Item, Record, FIELD_OFFSET(WHEAP_RECORD, Packet));
        ErrorSourceId = Record->Source->ErrorSourceId;
        CreatorId = Record->Source->CreatorId;
        PartitionId = Record->Source->PartitionId;
        ErrorType = 0;
        if (!Item.DeviceDriver)
        {
            ErrorType = Packet->ErrorType;
            Item.Severity = Packet->ErrorSeverity;
        }

        Length = Record->PacketLength;
        Sequence = ++Record->Source->LoggedCount;
        FromPool = Record->FromPool;
        if (WheapLogBuffer && Item.DeviceDriver)
        {
            SectionBytes = Item.SectionCount * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR);
            RtlCopyMemory(WheapLogBuffer + sizeof(WHEA_ERROR_RECORD_HEADER), Record->Packet, SectionBytes);
            RtlCopyMemory(WheapLogBuffer + sizeof(WHEA_ERROR_RECORD_HEADER) + SectionBytes, WheapRecordData(Record), Length);
        }
        else if (WheapLogBuffer)
        {
            RtlCopyMemory(WheapLogBuffer + sizeof(WHEA_ERROR_RECORD_HEADER) + sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR),
                          Packet,
                          Length);
        }

        if (!FromPool)
            InsertTailList(&Record->Source->FreeRecords, &Record->Link);
        WheapReleaseLock(OldIrql);

        if (FromPool)
            ExFreePoolWithTag(Record, WHEAP_TAG);
        WheapFreeRecords(&Discarded);

        /* FIXME: drop this line once ETW events reach the event log; a source in a fault loop must not fill the log */
        if (Sequence <= WHEAP_DEBUG_LINES || (Sequence % WHEAP_DEBUG_LINE_INTERVAL) == 0)
        {
            DPRINT1("WHEA: hardware error %lu from source %lu: type %lu, severity %d, %lu bytes\n",
                    Sequence, ErrorSourceId, ErrorType, Item.Severity, Length);
        }

        if (WheapLogBuffer && Item.DeviceDriver)
            WheapLogDeviceDriverData(&Item, &CreatorId, &PartitionId);
        else if (WheapLogBuffer)
            WheapLogPacket(Length);
    }
}

static
VOID
NTAPI
WheapDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ExInitializeWorkItem(&WheapWorkItem, WheapWorker, NULL);
    ExQueueWorkItem(&WheapWorkItem, DelayedWorkQueue);
}

static
VOID
WheapPrepareConsumer(VOID)
{
    if (!WheapConsumerReady)
    {
        KeInitializeDpc(&WheapDpc, WheapDpcRoutine, NULL);
        WheapConsumerReady = TRUE;
    }
}

static
ULONG
WheapAssignSourceId(
    _Inout_ PWHEAP_ERROR_SOURCE Source)
{
    do
    {
        Source->ErrorSourceId = WheapNextSourceId++;
    } while (Source->ErrorSourceId == WHEA_INVALID_ERR_SRC_ID || WheapFindSource(Source->ErrorSourceId));

    return Source->ErrorSourceId;
}

static
VOID
WheapStripRecords(
    _In_ PLIST_ENTRY Head,
    _In_ PWHEAP_ERROR_SOURCE Source,
    _Inout_ PLIST_ENTRY PoolRecords)
{
    PWHEAP_RECORD Record;
    PLIST_ENTRY Entry, Next;

    for (Entry = Head->Flink; Entry != Head; Entry = Next)
    {
        Next = Entry->Flink;
        Record = CONTAINING_RECORD(Entry, WHEAP_RECORD, Link);
        if (Record->Source == Source)
        {
            RemoveEntryList(Entry);
            if (Record->FromPool)
                InsertTailList(PoolRecords, Entry);
        }
    }
}

static
BOOLEAN
WheapHasOpenReport(
    _In_ PWHEAP_ERROR_SOURCE Source)
{
    PLIST_ENTRY Entry;

    for (Entry = WheapOpenList.Flink; Entry != &WheapOpenList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, WHEAP_RECORD, Link)->Source == Source)
            return TRUE;
    }

    return FALSE;
}

static
NTSTATUS
WheapDetachSource(
    _In_ ULONG ErrorSourceId,
    _In_ BOOLEAN DeviceDriver,
    _In_ BOOLEAN EvenIfInUse,
    _Out_ PWHEAP_ERROR_SOURCE *Detached)
{
    PWHEAP_ERROR_SOURCE Source;
    LIST_ENTRY PoolRecords;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    *Detached = NULL;
    InitializeListHead(&PoolRecords);
    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (!Source || Source->DeviceDriver != DeviceDriver)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (!EvenIfInUse && WheapHasOpenReport(Source))
    {
        Status = STATUS_RESOURCE_IN_USE;
    }
    else
    {
        RemoveEntryList(&Source->Link);
        WheapStripRecords(&WheapPendingList, Source, &PoolRecords);
        WheapStripRecords(&WheapOpenList, Source, &PoolRecords);
        *Detached = Source;
    }

    WheapReleaseLock(OldIrql);

    while (!IsListEmpty(&PoolRecords))
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&PoolRecords), WHEAP_RECORD, Link), WHEAP_TAG);

    return Status;
}

NTSTATUS
NTAPI
WheaAddErrorSource(
    _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
    _In_opt_ PVOID Context)
{
    WHEAP_DESCRIPTOR_HEADER Header;
    PWHEAP_ERROR_SOURCE Source;
    PWHEAP_RECORD Record;
    SIZE_T SourceSize, RecordSize;
    ULONG RecordCount, Index;
    KIRQL OldIrql;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    if (!ErrorSource)
        return STATUS_INVALID_PARAMETER;

    Header = *(PWHEAP_DESCRIPTOR_HEADER)ErrorSource;
    if (Header.Length < sizeof(Header) ||
        Header.Length > WHEAP_MAX_DESCRIPTOR_LENGTH ||
        Header.Version < WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10 ||
        (ULONG)Header.Type >= WheaErrSrcTypeMax ||
        (Header.State != WheaErrSrcStateStopped && Header.State != WheaErrSrcStateStarted) ||
        Header.MaxRawDataLength > WHEAP_MAX_RAW_DATA_LENGTH)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SourceSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_ERROR_SOURCE, Descriptor) + Header.Length, MEMORY_ALLOCATION_ALIGNMENT);
    RecordSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_RECORD, Packet) + sizeof(WHEA_ERROR_PACKET_V2) + Header.MaxRawDataLength,
                             MEMORY_ALLOCATION_ALIGNMENT);
    RecordCount = Header.NumRecordsToPreallocate;
    if (RecordCount > WHEAP_MAX_RECORDS)
        RecordCount = WHEAP_MAX_RECORDS;
    if (RecordCount > WHEAP_MAX_RECORD_BYTES / RecordSize)
        RecordCount = (ULONG)(WHEAP_MAX_RECORD_BYTES / RecordSize);
    if (RecordCount == 0)
        RecordCount = 1;

    Source = ExAllocatePoolWithTag(NonPagedPoolNx, SourceSize + RecordCount * RecordSize, WHEAP_TAG);
    if (!Source)
        return STATUS_INSUFFICIENT_RESOURCES;

    Source->State = Header.State;
    Source->Context = Context;
    Source->MaxPacketLength = sizeof(WHEA_ERROR_PACKET_V2) + Header.MaxRawDataLength;
    Source->ReportedCount = 0;
    Source->DroppedCount = 0;
    Source->LoggedCount = 0;
    Source->DeviceDriver = FALSE;
    Source->MaxSections = 0;
    Source->Uninitialize = NULL;
    RtlZeroMemory(&Source->CreatorId, sizeof(GUID));
    RtlZeroMemory(&Source->PartitionId, sizeof(GUID));
    InitializeListHead(&Source->FreeRecords);
    Source->DescriptorLength = Header.Length;
    RtlCopyMemory(Source->Descriptor, ErrorSource, Header.Length);

    Record = (PWHEAP_RECORD)((PUCHAR)Source + SourceSize);
    for (Index = 0; Index < RecordCount; Index++)
    {
        Record->Source = Source;
        Record->DeviceDriver = FALSE;
        Record->FromPool = FALSE;
        InsertTailList(&Source->FreeRecords, &Record->Link);
        Record = (PWHEAP_RECORD)((PUCHAR)Record + RecordSize);
    }

    WheapAcquireLock(&OldIrql);
    WheapPrepareConsumer();
    Header.ErrorSourceId = WheapAssignSourceId(Source);
    RtlCopyMemory(Source->Descriptor, &Header, sizeof(Header));
    InsertTailList(&WheapSourceList, &Source->Link);
    WheapReleaseLock(OldIrql);

    ((PWHEAP_DESCRIPTOR_HEADER)ErrorSource)->ErrorSourceId = Header.ErrorSourceId;
    return STATUS_SUCCESS;
}

VOID
NTAPI
WheaRemoveErrorSource(
    _In_ ULONG ErrorSourceId)
{
    PWHEAP_ERROR_SOURCE Source;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    WheapDetachSource(ErrorSourceId, FALSE, TRUE, &Source);
    if (Source)
        ExFreePoolWithTag(Source, WHEAP_TAG);
}

NTSTATUS
NTAPI
WheaReportHwError(
    _In_ PWHEA_ERROR_PACKET ErrorPacket)
{
    WHEA_ERROR_PACKET_V2 Header;
    PWHEAP_ERROR_SOURCE Source;
    PWHEAP_RECORD Record;
    BOOLEAN QueueDpc = FALSE;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!ErrorPacket)
        return STATUS_INVALID_PARAMETER;

    Header = *ErrorPacket;
    if (Header.Signature != WHEA_ERROR_PACKET_V2_SIGNATURE ||
        Header.Version != WHEA_ERROR_PACKET_V2_VERSION ||
        Header.Length < sizeof(Header) ||
        !WheapIsRangeInPacket(Header.DataOffset, Header.DataLength, Header.Length) ||
        !WheapIsRangeInPacket(Header.PshedDataOffset, Header.PshedDataLength, Header.Length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(Header.ErrorSourceId);
    if (!Source)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (Source->DeviceDriver)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Source->State != WheaErrSrcStateStarted)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
    }
    else if (Header.Length > Source->MaxPacketLength)
    {
        Status = STATUS_INVALID_BUFFER_SIZE;
    }
    else if (IsListEmpty(&Source->FreeRecords))
    {
        Source->DroppedCount++;
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        Record = CONTAINING_RECORD(RemoveHeadList(&Source->FreeRecords), WHEAP_RECORD, Link);
        RtlCopyMemory(Record->Packet, ErrorPacket, Header.Length);
        RtlCopyMemory(Record->Packet, &Header, sizeof(Header));
        Record->PacketLength = Header.Length;
        KeQuerySystemTime(&Record->SystemTime);
        InsertTailList(&WheapPendingList, &Record->Link);
        Source->ReportedCount++;
        if (!WheapWorkQueued)
        {
            WheapWorkQueued = TRUE;
            QueueDpc = TRUE;
        }

        Status = STATUS_SUCCESS;
    }

    WheapReleaseLock(OldIrql);

    if (QueueDpc)
        KeInsertQueueDpc(&WheapDpc, NULL, NULL);

    return Status;
}

static
NTSTATUS
WheapAddDeviceDriverSource(
    _In_opt_ PVOID Context,
    _In_ PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1 Configuration,
    _In_ ULONG ReportCount,
    _In_ ULONG MaxDataLength,
    _In_ ULONG MaxSections,
    _In_opt_ const GUID *CreatorId,
    _In_opt_ const GUID *PartitionId)
{
    WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER Initialize = Configuration->Initialize;
    PWHEAP_ERROR_SOURCE Source;
    PWHEAP_RECORD Record;
    SIZE_T SourceSize, RecordSize;
    ULONG ErrorSourceId, Index;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (MaxSections == 0)
        MaxSections = 1;

    if (!Initialize ||
        !Configuration->Uninitialize ||
        MaxDataLength > WHEAP_MAX_RAW_DATA_LENGTH ||
        MaxSections > WHEAP_MAX_SECTIONS ||
        MaxSections * MaxDataLength > WHEAP_MAX_RAW_DATA_LENGTH)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SourceSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_ERROR_SOURCE, Descriptor), MEMORY_ALLOCATION_ALIGNMENT);
    RecordSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_RECORD, Packet) +
                             MaxSections * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) + MaxSections * MaxDataLength,
                             MEMORY_ALLOCATION_ALIGNMENT);
    if (ReportCount > WHEAP_MAX_RECORDS)
        ReportCount = WHEAP_MAX_RECORDS;
    if (ReportCount > WHEAP_MAX_RECORD_BYTES / RecordSize)
        ReportCount = (ULONG)(WHEAP_MAX_RECORD_BYTES / RecordSize);

    Source = ExAllocatePoolWithTag(NonPagedPoolNx, SourceSize + ReportCount * RecordSize, WHEAP_TAG);
    if (!Source)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Source, SourceSize);
    Source->State = WheaErrSrcStateStarted;
    Source->Context = Context;
    Source->MaxPacketLength = MaxDataLength;
    Source->MaxSections = MaxSections;
    Source->DeviceDriver = TRUE;
    Source->Uninitialize = Configuration->Uninitialize;
    if (CreatorId)
        Source->CreatorId = *CreatorId;
    if (PartitionId)
        Source->PartitionId = *PartitionId;
    InitializeListHead(&Source->FreeRecords);

    Record = (PWHEAP_RECORD)((PUCHAR)Source + SourceSize);
    for (Index = 0; Index < ReportCount; Index++)
    {
        Record->Source = Source;
        Record->DeviceDriver = TRUE;
        Record->FromPool = FALSE;
        Record->SectionSlots = MaxSections;
        Record->DataCapacity = MaxSections * MaxDataLength;
        InsertTailList(&Source->FreeRecords, &Record->Link);
        Record = (PWHEAP_RECORD)((PUCHAR)Record + RecordSize);
    }

    WheapAcquireLock(&OldIrql);
    WheapPrepareConsumer();
    ErrorSourceId = WheapAssignSourceId(Source);
    InsertTailList(&WheapSourceList, &Source->Link);
    WheapReleaseLock(OldIrql);

    Status = Initialize(Context, ErrorSourceId);
    if (!NT_SUCCESS(Status))
    {
        WheapDetachSource(ErrorSourceId, TRUE, TRUE, &Source);
        if (Source)
            ExFreePoolWithTag(Source, WHEAP_TAG);
    }

    return Status;
}

NTSTATUS
NTAPI
WheaAddErrorSourceDeviceDriver(
    _Inout_opt_ PVOID Context,
    _In_ PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration,
    _In_ ULONG NumberPreallocatedErrorReports)
{
    WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Captured;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (!Configuration)
        return STATUS_INVALID_PARAMETER;

    Captured = *Configuration;
    if (Captured.Version != WHEA_DEVICE_DRIVER_CONFIG_V2)
        return STATUS_INVALID_PARAMETER_2;

    return WheapAddDeviceDriverSource(Context,
                                      (PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1)&Captured,
                                      NumberPreallocatedErrorReports,
                                      Captured.MaxSectionDataLength,
                                      Captured.MaxSectionsPerReport,
                                      &Captured.CreatorId,
                                      &Captured.PartitionId);
}

NTSTATUS
NTAPI
WheaAddErrorSourceDeviceDriverV1(
    _Inout_opt_ PVOID Context,
    _In_ PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration,
    _In_ ULONG NumBuffersToPreallocate,
    _In_ ULONG MaxDataLength)
{
    WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1 Captured;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (!Configuration)
        return STATUS_INVALID_PARAMETER;

    Captured = *(PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1)Configuration;
    if (Captured.Version != WHEA_DEVICE_DRIVER_CONFIG_V1)
        return STATUS_INVALID_PARAMETER_2;

    return WheapAddDeviceDriverSource(Context, &Captured, NumBuffersToPreallocate, MaxDataLength, 1, NULL, NULL);
}

static
BOOLEAN
WheapIsReportOpen(
    _In_ ULONG ErrorSourceId)
{
    PWHEAP_ERROR_SOURCE Source;
    BOOLEAN Open;
    KIRQL OldIrql;

    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    Open = Source && Source->DeviceDriver && WheapHasOpenReport(Source);
    WheapReleaseLock(OldIrql);
    return Open;
}

static
BOOLEAN
WheapHasPendingReport(
    _In_ ULONG ErrorSourceId)
{
    PWHEAP_RECORD Record;
    PLIST_ENTRY Entry;
    BOOLEAN Pending = FALSE;
    KIRQL OldIrql;

    WheapAcquireLock(&OldIrql);
    for (Entry = WheapPendingList.Flink; Entry != &WheapPendingList; Entry = Entry->Flink)
    {
        Record = CONTAINING_RECORD(Entry, WHEAP_RECORD, Link);
        if (Record->DeviceDriver && Record->Source->ErrorSourceId == ErrorSourceId)
        {
            Pending = TRUE;
            break;
        }
    }

    WheapReleaseLock(OldIrql);
    return Pending;
}

NTSTATUS
NTAPI
WheaRemoveErrorSourceDeviceDriver(
    _In_ ULONG ErrorSourceId)
{
    PWHEAP_ERROR_SOURCE Source;
    LARGE_INTEGER Interval;
    NTSTATUS Status;
    ULONG Attempt;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (WheapIsReportOpen(ErrorSourceId))
        return STATUS_RESOURCE_IN_USE;

    Interval.QuadPart = WHEAP_REMOVE_WAIT_INTERVAL;
    for (Attempt = 0; Attempt < WHEAP_REMOVE_WAIT_ATTEMPTS && WheapHasPendingReport(ErrorSourceId); Attempt++)
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    Status = WheapDetachSource(ErrorSourceId, TRUE, FALSE, &Source);
    if (!NT_SUCCESS(Status))
        return Status;

    Source->Uninitialize(Source->Context);
    ExFreePoolWithTag(Source, WHEAP_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WheaReportHwErrorDeviceDriver(
    _In_ ULONG ErrorSourceId,
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(ErrorDataLength) PUCHAR ErrorData,
    _In_ ULONG ErrorDataLength,
    _In_ LPGUID SectionTypeGuid,
    _In_ WHEA_ERROR_SEVERITY ErrorSeverity,
    _In_opt_ LPSTR DeviceFriendlyName)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    PWHEAP_ERROR_SOURCE Source;
    PWHEAP_RECORD Record = NULL, Spare = NULL;
    BOOLEAN QueueDpc = FALSE;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG Index;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!SectionTypeGuid ||
        (!ErrorData && ErrorDataLength != 0) ||
        ErrorDataLength > WHEAP_MAX_RAW_DATA_LENGTH ||
        (ULONG)ErrorSeverity > WheaErrSevInformational)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        Spare = ExAllocatePoolWithTag(NonPagedPoolNx,
                                      FIELD_OFFSET(WHEAP_RECORD, Packet) + sizeof(*Section) + ErrorDataLength,
                                      WHEAP_TAG);
    }

    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (!Source)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (!Source->DeviceDriver)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Source->State != WheaErrSrcStateStarted)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
    }
    else if (ErrorDataLength > Source->MaxPacketLength)
    {
        Status = STATUS_INVALID_BUFFER_SIZE;
    }
    else
    {
        if (!IsListEmpty(&Source->FreeRecords))
        {
            Record = CONTAINING_RECORD(RemoveHeadList(&Source->FreeRecords), WHEAP_RECORD, Link);
        }
        else if (Spare)
        {
            Record = Spare;
            Spare = NULL;
            Record->Source = Source;
            Record->FromPool = TRUE;
            Record->SectionSlots = 1;
            Record->DataCapacity = ErrorDataLength;
        }

        if (!Record)
        {
            Source->DroppedCount++;
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            Record->DeviceDriver = TRUE;
            Record->Severity = ErrorSeverity;
            Record->SectionCount = 1;
            Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)Record->Packet;
            RtlZeroMemory(Section, sizeof(*Section));
            Section->SectionLength = ErrorDataLength;
            Section->SectionType = *SectionTypeGuid;
            Section->SectionSeverity = ErrorSeverity;
            if (DeviceFriendlyName)
            {
                for (Index = 0; Index < sizeof(Section->FRUText) - 1 && DeviceFriendlyName[Index]; Index++)
                    Section->FRUText[Index] = DeviceFriendlyName[Index];
            }

            if (ErrorDataLength)
                RtlCopyMemory(WheapRecordData(Record), ErrorData, ErrorDataLength);
            Record->PacketLength = ErrorDataLength;
            KeQuerySystemTime(&Record->SystemTime);
            InsertTailList(&WheapPendingList, &Record->Link);
            Source->ReportedCount++;
            if (!WheapWorkQueued)
            {
                WheapWorkQueued = TRUE;
                QueueDpc = TRUE;
            }

            Status = STATUS_SUCCESS;
        }
    }

    WheapReleaseLock(OldIrql);

    if (Spare)
        ExFreePoolWithTag(Spare, WHEAP_TAG);
    if (QueueDpc)
        KeInsertQueueDpc(&WheapDpc, NULL, NULL);

    return Status;
}

WHEA_ERROR_SOURCE_STATE
NTAPI
WheaErrorSourceGetState(
    _In_ ULONG ErrorSourceId)
{
    PWHEAP_ERROR_SOURCE Source;
    WHEA_ERROR_SOURCE_STATE State = WheaErrSrcStateRemoved;
    KIRQL OldIrql;

    /* FIXME: the state reported for an id that is not in the table is not documented; removed is assumed */
    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (Source)
        State = Source->State;

    WheapReleaseLock(OldIrql);
    return State;
}

WHEA_ERROR_HANDLE
NTAPI
WheaCreateHwErrorReportDeviceDriver(
    _In_ ULONG ErrorSourceId,
    _In_opt_ PDEVICE_OBJECT DeviceObject)
{
    PWHEAP_ERROR_SOURCE Source;
    PWHEAP_RECORD Record = NULL, Spare;
    ULONG Slots = 0, Capacity = 0;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (Source && Source->DeviceDriver && Source->State == WheaErrSrcStateStarted)
    {
        Slots = Source->MaxSections;
        Capacity = Source->MaxSections * Source->MaxPacketLength;
        if (!IsListEmpty(&Source->FreeRecords))
        {
            Record = CONTAINING_RECORD(RemoveHeadList(&Source->FreeRecords), WHEAP_RECORD, Link);
            WheapOpenReport(Record);
        }
        else if (OldIrql != PASSIVE_LEVEL)
        {
            Source->DroppedCount++;
        }
    }

    WheapReleaseLock(OldIrql);

    if (Record || Slots == 0 || OldIrql != PASSIVE_LEVEL)
        return Record;

    Spare = ExAllocatePoolWithTag(NonPagedPoolNx,
                                  FIELD_OFFSET(WHEAP_RECORD, Packet) +
                                  Slots * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) + Capacity,
                                  WHEAP_TAG);
    if (!Spare)
        return WHEA_ERROR_HANDLE_INVALID;

    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (Source && Source->DeviceDriver && Source->State == WheaErrSrcStateStarted &&
        Source->MaxSections == Slots && Source->MaxSections * Source->MaxPacketLength == Capacity)
    {
        Record = Spare;
        Spare = NULL;
        Record->Source = Source;
        Record->FromPool = TRUE;
        Record->SectionSlots = Slots;
        Record->DataCapacity = Capacity;
        WheapOpenReport(Record);
    }

    WheapReleaseLock(OldIrql);

    if (Spare)
        ExFreePoolWithTag(Spare, WHEAP_TAG);

    return Record;
}

NTSTATUS
NTAPI
WheaAddHwErrorReportSectionDeviceDriver(
    _In_ WHEA_ERROR_HANDLE ErrorHandle,
    _In_ ULONG SectionDataLength,
    _Out_ PWHEA_DRIVER_BUFFER_SET BufferSet)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section = NULL;
    PWHEAP_RECORD Record;
    PUCHAR Data = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!BufferSet)
        return STATUS_INVALID_PARAMETER;

    WheapAcquireLock(&OldIrql);
    Record = WheapFindOpenReport(ErrorHandle);
    if (!Record)
    {
        Status = STATUS_INVALID_HANDLE;
    }
    else if (Record->SectionCount == Record->SectionSlots ||
             SectionDataLength > Record->Source->MaxPacketLength ||
             SectionDataLength > Record->DataCapacity - Record->PacketLength)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)Record->Packet + Record->SectionCount;
        Data = WheapRecordData(Record) + Record->PacketLength;
        RtlZeroMemory(Section, sizeof(*Section));
        Section->SectionLength = SectionDataLength;
        Section->SectionSeverity = Record->Severity;
        RtlZeroMemory(Data, SectionDataLength);
        Record->SectionCount++;
        Record->PacketLength += SectionDataLength;
        Status = STATUS_SUCCESS;
    }

    WheapReleaseLock(OldIrql);

    if (NT_SUCCESS(Status))
    {
        BufferSet->Version = WHEA_DEVICE_DRIVER_BUFFER_SET_V1;
        BufferSet->Data = Data;
        BufferSet->DataSize = SectionDataLength;
        BufferSet->SectionTypeGuid = (LPGUID)((PUCHAR)Section + FIELD_OFFSET(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR, SectionType));
        BufferSet->SectionFriendlyName = (PUCHAR)Section + FIELD_OFFSET(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR, FRUText);
        /* FIXME: what Flags of the buffer set refers to is not documented; the flags of the section descriptor are assumed */
        BufferSet->Flags = (PUCHAR)Section + FIELD_OFFSET(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR, Flags);
    }

    return Status;
}

NTSTATUS
NTAPI
WheaHwErrorReportAbandonDeviceDriver(
    _In_ WHEA_ERROR_HANDLE ErrorHandle)
{
    PWHEAP_RECORD Record, Discarded = NULL;
    BOOLEAN QueueDpc = FALSE;
    KIRQL OldIrql;

    WheapAcquireLock(&OldIrql);
    Record = WheapFindOpenReport(ErrorHandle);
    if (Record)
    {
        RemoveEntryList(&Record->Link);
        Discarded = WheapRecycleRecord(Record, OldIrql, &QueueDpc);
    }

    WheapReleaseLock(OldIrql);

    if (Discarded)
        ExFreePoolWithTag(Discarded, WHEAP_TAG);
    if (QueueDpc)
        KeInsertQueueDpc(&WheapDpc, NULL, NULL);

    return Record ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS
NTAPI
WheaHwErrorReportSubmitDeviceDriver(
    _In_ WHEA_ERROR_HANDLE ErrorHandle)
{
    PWHEAP_RECORD Record, Discarded = NULL;
    BOOLEAN QueueDpc = FALSE;
    NTSTATUS Status;
    KIRQL OldIrql;

    WheapAcquireLock(&OldIrql);
    Record = WheapFindOpenReport(ErrorHandle);
    if (!Record)
    {
        Status = STATUS_INVALID_HANDLE;
    }
    else if (Record->SectionCount == 0)
    {
        RemoveEntryList(&Record->Link);
        Discarded = WheapRecycleRecord(Record, OldIrql, &QueueDpc);
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        RemoveEntryList(&Record->Link);
        KeQuerySystemTime(&Record->SystemTime);
        InsertTailList(&WheapPendingList, &Record->Link);
        Record->Source->ReportedCount++;
        if (!WheapWorkQueued)
        {
            WheapWorkQueued = TRUE;
            QueueDpc = TRUE;
        }

        Status = STATUS_SUCCESS;
    }

    WheapReleaseLock(OldIrql);

    if (Discarded)
        ExFreePoolWithTag(Discarded, WHEAP_TAG);
    if (QueueDpc)
        KeInsertQueueDpc(&WheapDpc, NULL, NULL);

    return Status;
}

NTSTATUS
NTAPI
WheaHwErrorReportSetSeverityDeviceDriver(
    _In_ WHEA_ERROR_HANDLE ErrorHandle,
    _In_ WHEA_ERROR_SEVERITY ErrorSeverity)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    PWHEAP_RECORD Record;
    KIRQL OldIrql;
    ULONG Index;

    if ((ULONG)ErrorSeverity > WheaErrSevInformational)
        return STATUS_INVALID_PARAMETER;

    WheapAcquireLock(&OldIrql);
    Record = WheapFindOpenReport(ErrorHandle);
    if (Record)
    {
        Record->Severity = ErrorSeverity;
        Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)Record->Packet;
        for (Index = 0; Index < Record->SectionCount; Index++)
            Section[Index].SectionSeverity = ErrorSeverity;
    }

    WheapReleaseLock(OldIrql);
    return Record ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS
NTAPI
WheaHwErrorReportSetSectionNameDeviceDriver(
    _In_ PWHEA_DRIVER_BUFFER_SET BufferSet,
    _In_range_(0, WHEA_GENERIC_ENTRY_TEXT_LEN) ULONG NameLength,
    _In_reads_bytes_(NameLength) PUCHAR Name)
{
    if (!BufferSet ||
        !BufferSet->SectionFriendlyName ||
        NameLength > WHEA_GENERIC_ENTRY_TEXT_LEN ||
        (!Name && NameLength != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(BufferSet->SectionFriendlyName, WHEA_GENERIC_ENTRY_TEXT_LEN);
    if (NameLength)
        RtlCopyMemory(BufferSet->SectionFriendlyName, Name, NameLength);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
WheaConfigureErrorSource(
    _In_ WHEA_ERROR_SOURCE_TYPE SourceType,
    _In_ PWHEA_ERROR_SOURCE_CONFIGURATION Configuration)
{
    if ((ULONG)SourceType >= WheaErrSrcTypeMax || !Configuration)
        return STATUS_INVALID_PARAMETER;

    /* FIXME: when and how the callbacks of a configuration are called is not documented; no configuration is accepted */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
WheaUnconfigureErrorSource(
    _In_ WHEA_ERROR_SOURCE_TYPE SourceType)
{
    if ((ULONG)SourceType >= WheaErrSrcTypeMax)
        return STATUS_INVALID_PARAMETER;

    /* FIXME: no configuration can be registered, so there is none to take back */
    return STATUS_NOT_SUPPORTED;
}
