/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hardware error sources, reports queued and drained, and the header of an error record
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

#define UNTOUCHED_ID 0xDEADBEEF

/* The ten fixed members of an error source descriptor, then room standing in for its type-specific part */
typedef struct _TEST_DESCRIPTOR
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
    UCHAR Info[64];
} TEST_DESCRIPTOR, *PTEST_DESCRIPTOR;

typedef struct _TEST_PACKET
{
    WHEA_ERROR_PACKET_V2 Header;
    UCHAR Data[64];
} TEST_PACKET, *PTEST_PACKET;

static
VOID
InitDescriptor(
    _Out_ PTEST_DESCRIPTOR Descriptor)
{
    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->Length = sizeof(*Descriptor);
    Descriptor->Version = WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10;
    Descriptor->Type = WheaErrSrcTypeGeneric;
    Descriptor->State = WheaErrSrcStateStarted;
    Descriptor->MaxRawDataLength = sizeof(((PTEST_PACKET)0)->Data);
    Descriptor->NumRecordsToPreallocate = 2;
    Descriptor->MaxSectionsPerRecord = 1;
    Descriptor->ErrorSourceId = UNTOUCHED_ID;
}

static
VOID
InitPacket(
    _Out_ PTEST_PACKET Packet,
    _In_ ULONG ErrorSourceId)
{
    RtlZeroMemory(Packet, sizeof(*Packet));
    Packet->Header.Signature = WHEA_ERROR_PACKET_V2_SIGNATURE;
    Packet->Header.Version = WHEA_ERROR_PACKET_V2_VERSION;
    Packet->Header.Length = sizeof(*Packet);
    Packet->Header.ErrorSourceId = ErrorSourceId;
    Packet->Header.ErrorSourceType = WheaErrSrcTypeGeneric;
    Packet->Header.DataFormat = WheaDataFormatGeneric;
    Packet->Header.DataOffset = sizeof(Packet->Header);
    Packet->Header.DataLength = sizeof(Packet->Data);
}

static
NTSTATUS
Add(
    _Inout_ PTEST_DESCRIPTOR Descriptor)
{
    return WheaAddErrorSource((PWHEA_ERROR_SOURCE_DESCRIPTOR)Descriptor, NULL);
}

static
VOID
TestAddRefusals(VOID)
{
    TEST_DESCRIPTOR Descriptor;

    ok_eq_hex(WheaAddErrorSource(NULL, NULL), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.Length = FIELD_OFFSET(TEST_DESCRIPTOR, Info) - 1;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);
    ok_eq_hex(Descriptor.ErrorSourceId, UNTOUCHED_ID);

    InitDescriptor(&Descriptor);
    Descriptor.Length = 0x2001;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.Version = WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10 - 1;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.Type = WheaErrSrcTypeMax;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.State = WheaErrSrcStateRemoved;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.MaxRawDataLength = 0x10001;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);
    ok_eq_hex(Descriptor.ErrorSourceId, UNTOUCHED_ID);
}

static
VOID
TestReportRefusals(
    _In_ ULONG StartedId,
    _In_ ULONG StoppedId)
{
    TEST_DESCRIPTOR Small;
    TEST_PACKET Packet;
    NTSTATUS Status;

    ok_eq_hex(WheaReportHwError(NULL), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.Signature = 0;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.Version = WHEA_ERROR_PACKET_V2_VERSION - 1;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.Length = sizeof(Packet.Header) - 1;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.DataLength = sizeof(Packet.Data) + 1;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.DataOffset = sizeof(Packet.Header) - 1;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.DataOffset = 0xFFFFFFF0;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, StartedId);
    Packet.Header.PshedDataOffset = sizeof(Packet);
    Packet.Header.PshedDataLength = 1;
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);

    InitPacket(&Packet, 0);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_NOT_FOUND);

    InitPacket(&Packet, StoppedId);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_DEVICE_STATE);

    InitDescriptor(&Small);
    Small.MaxRawDataLength = sizeof(Packet.Data) - 1;
    Status = Add(&Small);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        InitPacket(&Packet, Small.ErrorSourceId);
        ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_BUFFER_SIZE);
        Packet.Header.Length--;
        Packet.Header.DataLength--;
        ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_SUCCESS);
        WheaRemoveErrorSource(Small.ErrorSourceId);
    }
}

static
VOID
TestRecordHeader(VOID)
{
    static const GUID NoGuid = { 0 };
    WHEA_ERROR_RECORD_HEADER First, Second;

    ok_eq_hex(WheaInitializeRecordHeader(NULL), STATUS_INVALID_PARAMETER);

    RtlFillMemory(&First, sizeof(First), 0xA5);
    ok_eq_hex(WheaInitializeRecordHeader(&First), STATUS_SUCCESS);
    ok_eq_hex(sizeof(First), 128);
    ok(RtlEqualMemory(&First.Signature, "CPER", 4), "The record does not start with CPER\n");
    ok_eq_hex(First.Revision.AsUSHORT, 0x0210);
    ok_eq_hex(First.SignatureEnd, 0xFFFFFFFF);
    ok_eq_hex(First.SectionCount, 0);
    ok_eq_hex(First.Length, sizeof(First));
    ok_eq_hex(First.ValidBits.AsULONG, 2);
    ok(First.Timestamp.Century >= 20 && First.Timestamp.Year < 100 &&
       First.Timestamp.Month >= 1 && First.Timestamp.Month <= 12 &&
       First.Timestamp.Day >= 1 && First.Timestamp.Day <= 31 &&
       First.Timestamp.Hours < 24 && First.Timestamp.Minutes < 60 && First.Timestamp.Seconds < 60,
       "Timestamp 0x%I64x is not a date and time in plain binary\n", First.Timestamp.AsLARGE_INTEGER.QuadPart);
    ok_eq_hex(First.Flags.AsULONG, 0);
    ok(First.PersistenceInfo.AsULONGLONG == 0, "Persistence information is set\n");
    ok(!RtlEqualMemory(&First.CreatorId, &NoGuid, sizeof(GUID)), "No creator\n");
    ok(RtlEqualMemory(&First.PlatformId, &NoGuid, sizeof(GUID)), "A platform id without its valid bit\n");

    ok_eq_hex(WheaInitializeRecordHeader(&Second), STATUS_SUCCESS);
    ok(Second.RecordId > First.RecordId, "Record ids 0x%I64x then 0x%I64x\n", First.RecordId, Second.RecordId);
    ok(RtlEqualMemory(&First.CreatorId, &Second.CreatorId, sizeof(GUID)), "The creator changed\n");
}

/* Reports until the worker has given a record back to the source, for at most five seconds */
static
NTSTATUS
ReportWhenDrained(
    _In_ PTEST_PACKET Packet)
{
    LARGE_INTEGER Interval;
    NTSTATUS Status;
    ULONG Attempt;

    Interval.QuadPart = -10 * 10000;
    for (Attempt = 0; Attempt < 500; Attempt++)
    {
        Status = WheaReportHwError(&Packet->Header);
        if (Status != STATUS_INSUFFICIENT_RESOURCES)
            break;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    return Status;
}

/* At DISPATCH_LEVEL the worker cannot be started from this processor, so the records of a source run out */
static
ULONG
ReportUntilRefused(
    _In_ PTEST_PACKET Packet,
    _In_ ULONG Limit,
    _Out_ PNTSTATUS LastStatus)
{
    ULONG Accepted;
    KIRQL Irql;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    for (Accepted = 0; Accepted < Limit; Accepted++)
    {
        *LastStatus = WheaReportHwError(&Packet->Header);
        if (*LastStatus != STATUS_SUCCESS)
            break;
    }

    KeLowerIrql(Irql);
    return Accepted;
}

START_TEST(WheaErrorSource)
{
    TEST_DESCRIPTOR Started, Stopped, Large;
    TEST_PACKET Packet;
    NTSTATUS Status, Last;
    ULONG FirstId;
    KIRQL Irql;

    TestRecordHeader();
    TestAddRefusals();

    InitDescriptor(&Started);
    Status = Add(&Started);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    FirstId = Started.ErrorSourceId;
    ok(FirstId != 0 && FirstId != UNTOUCHED_ID, "No id was written back: 0x%lx\n", FirstId);

    InitDescriptor(&Stopped);
    Stopped.State = WheaErrSrcStateStopped;
    Stopped.NumRecordsToPreallocate = 0;
    Status = Add(&Stopped);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        WheaRemoveErrorSource(FirstId);
        return;
    }

    ok(Stopped.ErrorSourceId != 0 && Stopped.ErrorSourceId != FirstId, "Second id 0x%lx, first 0x%lx\n", Stopped.ErrorSourceId, FirstId);

    TestReportRefusals(FirstId, Stopped.ErrorSourceId);

    /* Two records were asked for */
    InitPacket(&Packet, FirstId);
    Last = STATUS_SUCCESS;
    ok_eq_hex(ReportUntilRefused(&Packet, 8, &Last), 2);
    ok_eq_hex(Last, STATUS_INSUFFICIENT_RESOURCES);

    /* Below DISPATCH_LEVEL the worker takes the queued records and gives them back */
    ok_eq_hex(ReportWhenDrained(&Packet), STATUS_SUCCESS);
    ok_eq_hex(ReportWhenDrained(&Packet), STATUS_SUCCESS);
    ok_eq_hex(ReportWhenDrained(&Packet), STATUS_SUCCESS);

    /* A source removed while its records are queued takes them out of the queue */
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Status = WheaReportHwError(&Packet.Header);
    WheaRemoveErrorSource(FirstId);
    Last = WheaReportHwError(&Packet.Header);
    KeLowerIrql(Irql);
    ok(Status == STATUS_SUCCESS || Status == STATUS_INSUFFICIENT_RESOURCES, "Status 0x%lx\n", Status);
    ok_eq_hex(Last, STATUS_NOT_FOUND);
    WheaRemoveErrorSource(FirstId);

    InitDescriptor(&Started);
    Status = Add(&Started);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Started.ErrorSourceId != FirstId, "The id 0x%lx was handed out again at once\n", FirstId);
        InitPacket(&Packet, Started.ErrorSourceId);
        ok_eq_hex(ReportUntilRefused(&Packet, 8, &Last), 2);
        ok_eq_hex(Last, STATUS_INSUFFICIENT_RESOURCES);
        ok_eq_hex(ReportWhenDrained(&Packet), STATUS_SUCCESS);
        WheaRemoveErrorSource(Started.ErrorSourceId);
    }

    /* A source may ask for 64 records of the largest size; it gets what fits in 1 MB, which is 15 */
    InitDescriptor(&Large);
    Large.MaxRawDataLength = 0x10000;
    Large.NumRecordsToPreallocate = 64;
    Status = Add(&Large);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        InitPacket(&Packet, Large.ErrorSourceId);
        ok_eq_hex(ReportUntilRefused(&Packet, 64, &Last), 15);
        ok_eq_hex(Last, STATUS_INSUFFICIENT_RESOURCES);
        ok_eq_hex(ReportWhenDrained(&Packet), STATUS_SUCCESS);
        WheaRemoveErrorSource(Large.ErrorSourceId);
    }

    WheaRemoveErrorSource(Stopped.ErrorSourceId);
}
