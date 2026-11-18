/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hardware error sources of the platform and of device drivers, their reports and error records
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

    ok_eq_hex(WheaErrSrcStateStopped, 1);
    ok_eq_hex(WheaErrSrcStateStarted, 2);
    ok_eq_hex(WheaErrSrcStateRemoved, 3);
    ok_eq_hex(WheaErrSrcStateRemovePending, 4);
    ok_eq_hex(WheaErrTypePmem, 7);
    ok_eq_hex(sizeof(WHEA_XPF_MCA_SECTION), 1192);
    ok_eq_hex(FIELD_OFFSET(WHEA_XPF_MCA_SECTION, GlobalCapability), 264);

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

    ok_eq_hex(WheaErrSrcTypeIPFCPE, 0x0b);
    ok_eq_hex(WheaErrSrcTypeGenericV2, 0x0c);
    ok_eq_hex(WheaErrSrcTypeDeviceDriver, 0x10);
    ok_eq_hex(WheaErrSrcTypeSei, 0x12);
    ok_eq_hex(WheaErrSrcTypeMax, 0x13);

    InitDescriptor(&Descriptor);
    Descriptor.Type = WheaErrSrcTypeMax;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.Type = WheaErrSrcTypeSei;
    ok_eq_hex(Add(&Descriptor), STATUS_SUCCESS);
    ok(Descriptor.ErrorSourceId != UNTOUCHED_ID, "A source of the last type got no id\n");
    WheaRemoveErrorSource(Descriptor.ErrorSourceId);

    InitDescriptor(&Descriptor);
    Descriptor.State = WheaErrSrcStateRemoved;
    ok_eq_hex(Add(&Descriptor), STATUS_INVALID_PARAMETER);

    InitDescriptor(&Descriptor);
    Descriptor.State = (WHEA_ERROR_SOURCE_STATE)0;
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

static ULONG DriverInitializeCalls, DriverUninitializeCalls, DriverErrorSourceId;
static PVOID DriverContextSeen;
static NTSTATUS DriverInitializeStatus;
static const GUID DriverSectionType = { 0x0F2A6C7E, 0x51D3, 0x4F8B, { 0x9C, 0x11, 0x27, 0x3D, 0x5A, 0x40, 0xE6, 0xB2 } };

static
NTSTATUS
NTAPI
DriverInitialize(
    _Inout_opt_ PVOID Context,
    _In_ ULONG ErrorSourceId)
{
    DriverInitializeCalls++;
    DriverContextSeen = Context;
    DriverErrorSourceId = ErrorSourceId;
    return DriverInitializeStatus;
}

static
VOID
NTAPI
DriverUninitialize(
    _Inout_opt_ PVOID Context)
{
    DriverUninitializeCalls++;
    DriverContextSeen = Context;
}

static
NTSTATUS
DriverReport(
    _In_ ULONG ErrorSourceId,
    _In_ ULONG Length,
    _In_ WHEA_ERROR_SEVERITY Severity)
{
    static UCHAR Data[48];

    return WheaReportHwErrorDeviceDriver(ErrorSourceId, NULL, Data, Length, (LPGUID)&DriverSectionType, Severity, "test device");
}

/* A report at DISPATCH_LEVEL needs a preallocated record; this waits until the worker has given one back */
static
NTSTATUS
DriverReportWhenDrained(
    _In_ ULONG ErrorSourceId)
{
    LARGE_INTEGER Interval;
    NTSTATUS Status;
    ULONG Attempt;
    KIRQL Irql;

    Interval.QuadPart = -10 * 10000;
    for (Attempt = 0; Attempt < 500; Attempt++)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &Irql);
        Status = DriverReport(ErrorSourceId, 48, WheaErrSevCorrected);
        KeLowerIrql(Irql);
        if (Status != STATUS_INSUFFICIENT_RESOURCES)
            break;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    return Status;
}

static
VOID
TestDeviceDriverSource(VOID)
{
    WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration;
    TEST_DESCRIPTOR Platform;
    TEST_PACKET Packet;
    NTSTATUS Status, Last;
    ULONG Id, Accepted;
    KIRQL Irql;

    ok_eq_hex(sizeof(WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1), 28 + 2 * sizeof(PVOID));
    ok_eq_hex(sizeof(WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER), 68 + 2 * sizeof(PVOID));

    RtlZeroMemory(&Configuration, sizeof(Configuration));
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V2;
    Configuration.Initialize = DriverInitialize;
    Configuration.Uninitialize = DriverUninitialize;
    Configuration.MaxSectionDataLength = 48;
    Configuration.MaxSectionsPerReport = 1;
    DriverInitializeStatus = STATUS_SUCCESS;

    ok_eq_hex(WheaAddErrorSourceDeviceDriver(NULL, NULL, 0), STATUS_INVALID_PARAMETER);
    Configuration.Initialize = NULL;
    ok_eq_hex(WheaAddErrorSourceDeviceDriver(NULL, &Configuration, 0), STATUS_INVALID_PARAMETER);
    Configuration.Initialize = DriverInitialize;
    Configuration.Uninitialize = NULL;
    ok_eq_hex(WheaAddErrorSourceDeviceDriver(NULL, &Configuration, 0), STATUS_INVALID_PARAMETER);
    Configuration.Uninitialize = DriverUninitialize;
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V1;
    ok_eq_hex(WheaAddErrorSourceDeviceDriver(NULL, &Configuration, 0), STATUS_INVALID_PARAMETER_2);
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V2 + 1;
    ok_eq_hex(WheaAddErrorSourceDeviceDriver(NULL, &Configuration, 0), STATUS_INVALID_PARAMETER_2);
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V2;
    ok_eq_hex(WheaAddErrorSourceDeviceDriverV1(NULL, &Configuration, 0, 48), STATUS_INVALID_PARAMETER_2);
    ok_eq_hex(DriverInitializeCalls, 0);

    /* A failing initialize callback: its status comes back and the source is gone */
    DriverInitializeStatus = STATUS_DEVICE_NOT_READY;
    ok_eq_hex(WheaAddErrorSourceDeviceDriver(&Configuration, &Configuration, 0), STATUS_DEVICE_NOT_READY);
    ok_eq_hex(DriverInitializeCalls, 1);
    ok(DriverErrorSourceId != WHEA_INVALID_ERR_SRC_ID, "The callback got no id\n");
    ok_eq_hex(WheaErrorSourceGetState(DriverErrorSourceId), WheaErrSrcStateRemoved);
    ok_eq_hex(DriverUninitializeCalls, 0);

    /* Nothing preallocated: reports work at PASSIVE_LEVEL only */
    DriverInitializeStatus = STATUS_SUCCESS;
    DriverContextSeen = NULL;
    Status = WheaAddErrorSourceDeviceDriver(&Configuration, &Configuration, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Id = DriverErrorSourceId;
    ok_eq_hex(DriverInitializeCalls, 2);
    ok(DriverContextSeen == &Configuration, "The callback got another context\n");
    ok_eq_hex(WheaErrorSourceGetState(Id), WheaErrSrcStateStarted);

    ok_eq_hex(WheaReportHwErrorDeviceDriver(Id, NULL, (PUCHAR)&Packet, 48, NULL, WheaErrSevCorrected, NULL), STATUS_INVALID_PARAMETER);
    ok_eq_hex(DriverReport(Id, 48, (WHEA_ERROR_SEVERITY)(WheaErrSevInformational + 1)), STATUS_INVALID_PARAMETER);
    ok_eq_hex(DriverReport(WHEA_INVALID_ERR_SRC_ID, 48, WheaErrSevCorrected), STATUS_NOT_FOUND);
    ok_eq_hex(DriverReport(Id, 49, WheaErrSevCorrected), STATUS_INVALID_BUFFER_SIZE);
    ok_eq_hex(DriverReport(Id, 48, WheaErrSevCorrected), STATUS_SUCCESS);
    ok_eq_hex(DriverReport(Id, 0, WheaErrSevFatal), STATUS_SUCCESS);
    ok_eq_hex(DriverReport(Id, 48, WheaErrSevInformational), STATUS_SUCCESS);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Status = DriverReport(Id, 48, WheaErrSevCorrected);
    KeLowerIrql(Irql);
    ok_eq_hex(Status, STATUS_INSUFFICIENT_RESOURCES);

    /* The two families do not take each other's sources */
    InitPacket(&Packet, Id);
    ok_eq_hex(WheaReportHwError(&Packet.Header), STATUS_INVALID_PARAMETER);
    WheaRemoveErrorSource(Id);
    ok_eq_hex(WheaErrorSourceGetState(Id), WheaErrSrcStateStarted);
    InitDescriptor(&Platform);
    Status = Add(&Platform);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_hex(DriverReport(Platform.ErrorSourceId, 48, WheaErrSevCorrected), STATUS_INVALID_PARAMETER);
        ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Platform.ErrorSourceId), STATUS_NOT_FOUND);
        ok_eq_hex(WheaErrorSourceGetState(Platform.ErrorSourceId), WheaErrSrcStateStarted);
        WheaRemoveErrorSource(Platform.ErrorSourceId);
    }

    DriverContextSeen = NULL;
    ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Id), STATUS_SUCCESS);
    ok_eq_hex(DriverUninitializeCalls, 1);
    ok(DriverContextSeen == &Configuration, "The uninitialize callback got another context\n");
    ok_eq_hex(WheaErrorSourceGetState(Id), WheaErrSrcStateRemoved);
    ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Id), STATUS_NOT_FOUND);
    ok_eq_hex(DriverReport(Id, 48, WheaErrSevCorrected), STATUS_NOT_FOUND);

    /* Version 1 of the configuration, two preallocated reports: exact at DISPATCH_LEVEL */
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V1;
    Status = WheaAddErrorSourceDeviceDriverV1(NULL, &Configuration, 2, 48);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Id = DriverErrorSourceId;
    Last = STATUS_SUCCESS;
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    for (Accepted = 0; Accepted < 8; Accepted++)
    {
        Last = DriverReport(Id, 48, WheaErrSevCorrected);
        if (Last != STATUS_SUCCESS)
            break;
    }

    KeLowerIrql(Irql);
    ok_eq_hex(Accepted, 2);
    ok_eq_hex(Last, STATUS_INSUFFICIENT_RESOURCES);
    ok_eq_hex(DriverReportWhenDrained(Id), STATUS_SUCCESS);
    ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Id), STATUS_SUCCESS);
    ok_eq_hex(DriverUninitializeCalls, 2);
}

/* A report opened at DISPATCH_LEVEL needs a preallocated one; this waits until the worker has given it back */
static
WHEA_ERROR_HANDLE
CreateWhenDrained(
    _In_ ULONG ErrorSourceId)
{
    WHEA_ERROR_HANDLE Handle = WHEA_ERROR_HANDLE_INVALID;
    LARGE_INTEGER Interval;
    ULONG Attempt;
    KIRQL Irql;

    Interval.QuadPart = -10 * 10000;
    for (Attempt = 0; Attempt < 500; Attempt++)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &Irql);
        Handle = WheaCreateHwErrorReportDeviceDriver(ErrorSourceId, NULL);
        KeLowerIrql(Irql);
        if (Handle != WHEA_ERROR_HANDLE_INVALID)
            break;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    return Handle;
}

static
VOID
TestErrorReportHandle(VOID)
{
    static UCHAR SectionName[] = "test device";
    WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration;
    WHEA_DRIVER_BUFFER_SET First, Second, Third;
    WHEA_ERROR_HANDLE Handle, Other;
    TEST_DESCRIPTOR Platform;
    NTSTATUS Status;
    ULONG Id, Calls;
    KIRQL Irql;

    ok_eq_hex(sizeof(WHEA_DRIVER_BUFFER_SET), 8 + 4 * sizeof(PVOID));

    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(WHEA_ERROR_HANDLE_INVALID, 8, &First), STATUS_INVALID_HANDLE);
    ok_eq_hex(WheaHwErrorReportSubmitDeviceDriver(WHEA_ERROR_HANDLE_INVALID), STATUS_INVALID_HANDLE);
    ok_eq_hex(WheaHwErrorReportAbandonDeviceDriver(WHEA_ERROR_HANDLE_INVALID), STATUS_INVALID_HANDLE);
    ok_eq_hex(WheaHwErrorReportSetSeverityDeviceDriver(WHEA_ERROR_HANDLE_INVALID, WheaErrSevCorrected), STATUS_INVALID_HANDLE);
    ok(WheaCreateHwErrorReportDeviceDriver(WHEA_INVALID_ERR_SRC_ID, NULL) == WHEA_ERROR_HANDLE_INVALID, "A report for no source\n");

    RtlZeroMemory(&Configuration, sizeof(Configuration));
    Configuration.Version = WHEA_DEVICE_DRIVER_CONFIG_V2;
    Configuration.Initialize = DriverInitialize;
    Configuration.Uninitialize = DriverUninitialize;
    Configuration.MaxSectionDataLength = 48;
    Configuration.MaxSectionsPerReport = 2;
    DriverInitializeStatus = STATUS_SUCCESS;
    Calls = DriverUninitializeCalls;
    Status = WheaAddErrorSourceDeviceDriver(NULL, &Configuration, 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Id = DriverErrorSourceId;

    InitDescriptor(&Platform);
    Status = Add(&Platform);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(WheaCreateHwErrorReportDeviceDriver(Platform.ErrorSourceId, NULL) == WHEA_ERROR_HANDLE_INVALID,
           "A report for a platform source\n");
        WheaRemoveErrorSource(Platform.ErrorSourceId);
    }

    Handle = WheaCreateHwErrorReportDeviceDriver(Id, NULL);
    ok(Handle != WHEA_ERROR_HANDLE_INVALID, "No report\n");
    if (Handle == WHEA_ERROR_HANDLE_INVALID)
    {
        WheaRemoveErrorSourceDeviceDriver(Id);
        return;
    }

    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 8, NULL), STATUS_INVALID_PARAMETER);
    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 49, &First), STATUS_INSUFFICIENT_RESOURCES);
    RtlZeroMemory(&First, sizeof(First));
    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 48, &First), STATUS_SUCCESS);
    ok_eq_hex(First.Version, WHEA_DEVICE_DRIVER_BUFFER_SET_V1);
    ok_eq_hex(First.DataSize, 48);
    ok(First.Data && First.SectionTypeGuid && First.SectionFriendlyName, "A buffer of the first section is missing\n");
    if (First.Data && First.SectionTypeGuid && First.SectionFriendlyName)
    {
        ok(First.Data[0] == 0 && First.Data[47] == 0, "The data buffer does not start out clear\n");
        RtlFillMemory(First.Data, 48, 0x5A);
        *First.SectionTypeGuid = DriverSectionType;
        ok_eq_hex(WheaHwErrorReportSetSectionNameDeviceDriver(&First, WHEA_GENERIC_ENTRY_TEXT_LEN + 1, SectionName),
                  STATUS_INVALID_PARAMETER);
        ok_eq_hex(WheaHwErrorReportSetSectionNameDeviceDriver(NULL, 4, SectionName), STATUS_INVALID_PARAMETER);
        ok_eq_hex(WheaHwErrorReportSetSectionNameDeviceDriver(&First, sizeof(SectionName) - 1, SectionName), STATUS_SUCCESS);
        ok(RtlEqualMemory(First.SectionFriendlyName, SectionName, sizeof(SectionName)), "The name is not in the section\n");
    }

    RtlZeroMemory(&Second, sizeof(Second));
    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 0, &Second), STATUS_SUCCESS);
    ok_eq_hex(Second.DataSize, 0);
    ok(Second.SectionTypeGuid && Second.SectionTypeGuid != First.SectionTypeGuid, "Two sections share one header\n");
    if (Second.SectionTypeGuid)
        *Second.SectionTypeGuid = DriverSectionType;
    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 0, &Third), STATUS_INSUFFICIENT_RESOURCES);

    ok_eq_hex(WheaHwErrorReportSetSeverityDeviceDriver(Handle, (WHEA_ERROR_SEVERITY)(WheaErrSevInformational + 1)),
              STATUS_INVALID_PARAMETER);
    ok_eq_hex(WheaHwErrorReportSetSeverityDeviceDriver(Handle, WheaErrSevCorrected), STATUS_SUCCESS);

    /* The source stays while a report is open */
    ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Id), STATUS_RESOURCE_IN_USE);
    ok_eq_hex(WheaErrorSourceGetState(Id), WheaErrSrcStateStarted);
    ok_eq_hex(DriverUninitializeCalls, Calls);

    /* Submitting uses the handle up, also when the report is refused for having no section */
    ok_eq_hex(WheaHwErrorReportSubmitDeviceDriver(Handle), STATUS_SUCCESS);
    ok_eq_hex(WheaHwErrorReportSubmitDeviceDriver(Handle), STATUS_INVALID_HANDLE);

    Handle = WheaCreateHwErrorReportDeviceDriver(Id, NULL);
    ok(Handle != WHEA_ERROR_HANDLE_INVALID, "No second report\n");
    ok_eq_hex(WheaHwErrorReportSubmitDeviceDriver(Handle), STATUS_BUFFER_TOO_SMALL);
    ok_eq_hex(WheaHwErrorReportAbandonDeviceDriver(Handle), STATUS_INVALID_HANDLE);

    Handle = WheaCreateHwErrorReportDeviceDriver(Id, NULL);
    ok(Handle != WHEA_ERROR_HANDLE_INVALID, "No third report\n");
    ok_eq_hex(WheaHwErrorReportAbandonDeviceDriver(Handle), STATUS_SUCCESS);
    ok_eq_hex(WheaHwErrorReportAbandonDeviceDriver(Handle), STATUS_INVALID_HANDLE);
    ok_eq_hex(WheaAddHwErrorReportSectionDeviceDriver(Handle, 8, &First), STATUS_INVALID_HANDLE);

    /* One preallocated report: exactly one handle at DISPATCH_LEVEL */
    Handle = CreateWhenDrained(Id);
    ok(Handle != WHEA_ERROR_HANDLE_INVALID, "The preallocated report did not come back\n");
    Status = STATUS_UNSUCCESSFUL;
    RtlZeroMemory(&First, sizeof(First));
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Other = WheaCreateHwErrorReportDeviceDriver(Id, NULL);
    if (Handle != WHEA_ERROR_HANDLE_INVALID)
        Status = WheaAddHwErrorReportSectionDeviceDriver(Handle, 16, &First);
    KeLowerIrql(Irql);
    ok(Other == WHEA_ERROR_HANDLE_INVALID, "Two reports at DISPATCH_LEVEL from one preallocated\n");
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Other != WHEA_ERROR_HANDLE_INVALID)
        WheaHwErrorReportAbandonDeviceDriver(Other);
    if (NT_SUCCESS(Status) && First.SectionTypeGuid)
    {
        *First.SectionTypeGuid = DriverSectionType;
        ok_eq_hex(WheaHwErrorReportSubmitDeviceDriver(Handle), STATUS_SUCCESS);
    }
    else if (Handle != WHEA_ERROR_HANDLE_INVALID)
    {
        WheaHwErrorReportAbandonDeviceDriver(Handle);
    }

    ok_eq_hex(WheaRemoveErrorSourceDeviceDriver(Id), STATUS_SUCCESS);
    ok_eq_hex(DriverUninitializeCalls, Calls + 1);
}

static
VOID
TestConfiguration(VOID)
{
    WHEA_ERROR_SOURCE_CONFIGURATION Configuration;

    ok_eq_hex(sizeof(Configuration), 7 * sizeof(PVOID));
    RtlZeroMemory(&Configuration, sizeof(Configuration));
    ok_eq_hex(WheaConfigureErrorSource(WheaErrSrcTypeMax, &Configuration), STATUS_INVALID_PARAMETER);
    ok_eq_hex(WheaConfigureErrorSource(WheaErrSrcTypeGeneric, NULL), STATUS_INVALID_PARAMETER);
    ok_eq_hex(WheaUnconfigureErrorSource(WheaErrSrcTypeMax), STATUS_INVALID_PARAMETER);

    /* LiberNT accepts no configuration yet: the contract of its callbacks is not published */
    ok_eq_hex(WheaConfigureErrorSource(WheaErrSrcTypeGeneric, &Configuration), STATUS_NOT_SUPPORTED);
    ok_eq_hex(WheaUnconfigureErrorSource(WheaErrSrcTypeGeneric), STATUS_NOT_SUPPORTED);
}

START_TEST(WheaErrorSource)
{
    TEST_DESCRIPTOR Started, Stopped, Large;
    TEST_PACKET Packet;
    NTSTATUS Status, Last;
    ULONG FirstId;
    KIRQL Irql;

    TestRecordHeader();
    TestDeviceDriverSource();
    TestErrorReportHandle();
    TestConfiguration();
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
