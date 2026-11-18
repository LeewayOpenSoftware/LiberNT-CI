/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hardware error sources: add, remove, and reports queued in the records of their source
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

START_TEST(WheaErrorSource)
{
    TEST_DESCRIPTOR Started, Stopped;
    TEST_PACKET Packet;
    NTSTATUS Status;
    ULONG FirstId;
    KIRQL Irql;

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

    /* Two records were asked for: the third report finds none free, since nothing consumes the queue yet */
    InitPacket(&Packet, FirstId);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_SUCCESS);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Status = WheaReportHwError(&Packet.Header);
    KeLowerIrql(Irql);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INSUFFICIENT_RESOURCES);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INSUFFICIENT_RESOURCES);

    /* Removing a source with queued records leaves the queue usable for the others */
    WheaRemoveErrorSource(FirstId);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_NOT_FOUND);
    WheaRemoveErrorSource(FirstId);

    InitDescriptor(&Started);
    Status = Add(&Started);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Started.ErrorSourceId != FirstId, "The id 0x%lx was handed out again at once\n", FirstId);
        InitPacket(&Packet, Started.ErrorSourceId);
        ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_SUCCESS);
        ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_SUCCESS);
        ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INSUFFICIENT_RESOURCES);
        WheaRemoveErrorSource(Started.ErrorSourceId);
    }

    WheaRemoveErrorSource(Stopped.ErrorSourceId);
}
