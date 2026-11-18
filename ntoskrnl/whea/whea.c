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
#define WHEAP_EVENT_CHANNEL_SYSTEM 8
#define WHEAP_EVENT_KEYWORDS 0x8000000000000002ULL
#define WHEAP_LOG_BUFFER_LENGTH \
    (sizeof(WHEA_ERROR_RECORD_HEADER) + sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) + \
     sizeof(WHEA_ERROR_PACKET_V2) + WHEAP_MAX_RAW_DATA_LENGTH)

C_ASSERT(sizeof(WHEA_ERROR_RECORD_HEADER) == 128);
C_ASSERT(sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) == 72);

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
    LIST_ENTRY FreeRecords;
    ULONG DescriptorLength;
    UCHAR Descriptor[ANYSIZE_ARRAY];
} WHEAP_ERROR_SOURCE;

static KSPIN_LOCK WheapLock;
static LIST_ENTRY WheapSourceList = { &WheapSourceList, &WheapSourceList };
static LIST_ENTRY WheapPendingList = { &WheapPendingList, &WheapPendingList };
static ULONG WheapNextSourceId = 1;
static KDPC WheapDpc;
static WORK_QUEUE_ITEM WheapWorkItem;
static BOOLEAN WheapConsumerReady;
static BOOLEAN WheapWorkQueued;
static LONG64 WheapLastRecordId;

/* Used by the worker only */
static PUCHAR WheapLogBuffer;
static REGHANDLE WheapEtwHandle;
static BOOLEAN WheapEtwRegistered;

/* Microsoft-Windows-WHEA-Logger */
static const GUID WheapLoggerGuid = { 0xC26C4F3C, 0x3F66, 0x4E99, { 0x8F, 0x8A, 0x39, 0x40, 0x5C, 0xFE, 0xD2, 0x20 } };

/* Ours: the creator of the records made here, and the type of a section that holds a packet as reported */
static const GUID WheapCreatorGuid = { 0xE1E6D3CF, 0xDA8A, 0x48A3, { 0x9A, 0x7F, 0x3A, 0xD5, 0xFA, 0x13, 0xCC, 0xC0 } };
static const GUID WheapPacketSectionGuid = { 0x5BD0359E, 0x523C, 0x41FC, { 0xA3, 0x08, 0x9A, 0x51, 0x23, 0xE0, 0xE4, 0x2A } };

/* Taken at HIGH_LEVEL so that a driver can report from its interrupt routine */
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
    Header->CreatorId = WheapCreatorGuid;
    Header->RecordId = (ULONGLONG)InterlockedIncrement64(&WheapLastRecordId);

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

/* The record is the header, one section descriptor and the packet, which is already in the buffer */
static
VOID
WheapLogPacket(
    _In_ ULONG PacketLength)
{
    PWHEA_ERROR_RECORD_HEADER Header = (PWHEA_ERROR_RECORD_HEADER)WheapLogBuffer;
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)(Header + 1);
    PWHEA_ERROR_PACKET_V2 Packet = (PWHEA_ERROR_PACKET_V2)(Section + 1);
    EVENT_DATA_DESCRIPTOR Data[2];
    EVENT_DESCRIPTOR Event;
    ULONG Length;

    WheaInitializeRecordHeader(Header);
    Header->SectionCount = 1;
    Header->Severity = Packet->ErrorSeverity;
    Header->NotifyType = Packet->NotifyType;
    Header->Flags.PreviousError = Packet->Flags.PreviousError;
    Header->Flags.Simulated = Packet->Flags.Simulated;
    Length = sizeof(*Header) + sizeof(*Section) + PacketLength;
    Header->Length = Length;

    RtlZeroMemory(Section, sizeof(*Section));
    Section->SectionOffset = sizeof(*Header) + sizeof(*Section);
    Section->SectionLength = PacketLength;
    Section->Revision.AsUSHORT = WHEAP_SECTION_REVISION;
    Section->SectionType = WheapPacketSectionGuid;
    Section->SectionSeverity = Packet->ErrorSeverity;

    if (!WheapEtwRegistered)
        WheapEtwRegistered = NT_SUCCESS(EtwRegister(&WheapLoggerGuid, NULL, NULL, &WheapEtwHandle));
    if (!WheapEtwRegistered)
        return;

    /* Event 1 is an error with a record, 2 a corrected error, 3 information; a recoverable error goes with 1 */
    RtlZeroMemory(&Event, sizeof(Event));
    Event.Channel = WHEAP_EVENT_CHANNEL_SYSTEM;
    Event.Keyword = WHEAP_EVENT_KEYWORDS;
    if (Packet->ErrorSeverity == WheaErrSevCorrected)
    {
        Event.Id = 2;
        Event.Level = 3;
    }
    else if (Packet->ErrorSeverity == WheaErrSevInformational)
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
    WHEA_ERROR_TYPE ErrorType;
    WHEA_ERROR_SEVERITY Severity;
    ULONG ErrorSourceId, Length;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Parameter);

    if (!WheapLogBuffer)
        WheapLogBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, WHEAP_LOG_BUFFER_LENGTH, WHEAP_TAG);

    for (;;)
    {
        WheapAcquireLock(&OldIrql);
        if (IsListEmpty(&WheapPendingList))
        {
            WheapWorkQueued = FALSE;
            WheapReleaseLock(OldIrql);
            break;
        }

        Record = CONTAINING_RECORD(RemoveHeadList(&WheapPendingList), WHEAP_RECORD, Link);
        Packet = (PWHEA_ERROR_PACKET_V2)Record->Packet;
        ErrorSourceId = Packet->ErrorSourceId;
        ErrorType = Packet->ErrorType;
        Severity = Packet->ErrorSeverity;
        Length = Record->PacketLength;
        if (WheapLogBuffer)
        {
            RtlCopyMemory(WheapLogBuffer + sizeof(WHEA_ERROR_RECORD_HEADER) + sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR),
                          Packet,
                          Length);
        }

        InsertTailList(&Record->Source->FreeRecords, &Record->Link);
        WheapReleaseLock(OldIrql);

        /* FIXME: drop this line once ETW events reach the event log; until then it is the only trace */
        DPRINT1("WHEA: hardware error from source %lu: type %d, severity %d, packet of %lu bytes\n",
                ErrorSourceId, ErrorType, Severity, Length);
        if (WheapLogBuffer)
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

    /* A work item that has run cannot be queued again as it is; nothing queues this one while WheapWorkQueued is set */
    ExInitializeWorkItem(&WheapWorkItem, WheapWorker, NULL);
    ExQueueWorkItem(&WheapWorkItem, DelayedWorkQueue);
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
    InitializeListHead(&Source->FreeRecords);
    Source->DescriptorLength = Header.Length;
    RtlCopyMemory(Source->Descriptor, ErrorSource, Header.Length);

    Record = (PWHEAP_RECORD)((PUCHAR)Source + SourceSize);
    for (Index = 0; Index < RecordCount; Index++)
    {
        Record->Source = Source;
        InsertTailList(&Source->FreeRecords, &Record->Link);
        Record = (PWHEAP_RECORD)((PUCHAR)Record + RecordSize);
    }

    WheapAcquireLock(&OldIrql);
    if (!WheapConsumerReady)
    {
        KeInitializeDpc(&WheapDpc, WheapDpcRoutine, NULL);
        WheapConsumerReady = TRUE;
    }

    do
    {
        Source->ErrorSourceId = WheapNextSourceId++;
    } while (Source->ErrorSourceId == 0 || WheapFindSource(Source->ErrorSourceId));

    Header.ErrorSourceId = Source->ErrorSourceId;
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
    PLIST_ENTRY Entry, Next;
    KIRQL OldIrql;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    WheapAcquireLock(&OldIrql);
    Source = WheapFindSource(ErrorSourceId);
    if (Source)
    {
        RemoveEntryList(&Source->Link);
        for (Entry = WheapPendingList.Flink; Entry != &WheapPendingList; Entry = Next)
        {
            Next = Entry->Flink;
            if (CONTAINING_RECORD(Entry, WHEAP_RECORD, Link)->Source == Source)
                RemoveEntryList(Entry);
        }
    }

    WheapReleaseLock(OldIrql);

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
