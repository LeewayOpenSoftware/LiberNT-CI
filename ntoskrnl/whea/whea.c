/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hardware error sources and the queue of reported hardware errors
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define WHEAP_TAG 'aehW'
#define WHEAP_MAX_DESCRIPTOR_LENGTH 0x2000
#define WHEAP_MAX_RAW_DATA_LENGTH 0x10000
#define WHEAP_MAX_RECORDS 64

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

    RecordCount = Header.NumRecordsToPreallocate;
    if (RecordCount == 0)
        RecordCount = 1;
    if (RecordCount > WHEAP_MAX_RECORDS)
        RecordCount = WHEAP_MAX_RECORDS;

    SourceSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_ERROR_SOURCE, Descriptor) + Header.Length, MEMORY_ALLOCATION_ALIGNMENT);
    RecordSize = ALIGN_UP_BY(FIELD_OFFSET(WHEAP_RECORD, Packet) + sizeof(WHEA_ERROR_PACKET_V2) + Header.MaxRawDataLength,
                             MEMORY_ALLOCATION_ALIGNMENT);
    Source = ExAllocatePoolWithTag(NonPagedPool, SourceSize + RecordCount * RecordSize, WHEAP_TAG);
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
    do
    {
        Source->ErrorSourceId = WheapNextSourceId++;
    } while (Source->ErrorSourceId == 0 || WheapFindSource(Source->ErrorSourceId));

    InsertTailList(&WheapSourceList, &Source->Link);
    WheapReleaseLock(OldIrql);

    Header.ErrorSourceId = Source->ErrorSourceId;
    RtlCopyMemory(Source->Descriptor, &Header, sizeof(Header));
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
        Status = STATUS_SUCCESS;
    }

    WheapReleaseLock(OldIrql);
    return Status;
}
