/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A reported hardware error reaches an ETW session as an event of the WHEA logger provider
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#define _WMIKM_
#include <ntwmi.h>

NTSTATUS NTAPI WmiStartTrace(_Inout_ PWMI_LOGGER_INFORMATION LoggerInformation);
NTSTATUS NTAPI WmiStopTrace(_Inout_ PWMI_LOGGER_INFORMATION LoggerInformation);

#define WHEA_ETW_TAG 'ewhW'
#define REPORT_COUNT 4
#define CHECKED_REPORTS 2

static GUID WheaLoggerGuid = { 0xC26C4F3C, 0x3F66, 0x4E99, { 0x8F, 0x8A, 0x39, 0x40, 0x5C, 0xFE, 0xD2, 0x20 } };
static const GUID PacketSectionGuid = { 0xE71254E9, 0xC1B9, 0x4940, { 0xAB, 0x76, 0x90, 0x97, 0x03, 0xA4, 0x32, 0x0F } };

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

/* Report number 0 is a corrected error, 1 a fatal one; the later ones only push the first two through the worker */
static const WHEA_ERROR_SEVERITY ReportSeverity[REPORT_COUNT] =
{
    WheaErrSevCorrected, WheaErrSevFatal, WheaErrSevInformational, WheaErrSevInformational
};

static const USHORT ReportEventId[REPORT_COUNT] = { 2, 1, 3, 3 };

/* The source has one record: a report is accepted only once the worker has taken the previous one */
static
NTSTATUS
ReportWhenTaken(
    _In_ ULONG ErrorSourceId,
    _In_ ULONG Number)
{
    LARGE_INTEGER Interval;
    TEST_PACKET Packet;
    NTSTATUS Status;
    ULONG Attempt, Index;

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Header.Signature = WHEA_ERROR_PACKET_V2_SIGNATURE;
    Packet.Header.Version = WHEA_ERROR_PACKET_V2_VERSION;
    Packet.Header.Length = sizeof(Packet);
    Packet.Header.ErrorSourceId = ErrorSourceId;
    Packet.Header.ErrorSourceType = WheaErrSrcTypeGeneric;
    Packet.Header.ErrorSeverity = ReportSeverity[Number];
    Packet.Header.DataFormat = WheaDataFormatGeneric;
    Packet.Header.DataOffset = sizeof(Packet.Header);
    Packet.Header.DataLength = sizeof(Packet.Data);
    for (Index = 0; Index < sizeof(Packet.Data); Index++)
        Packet.Data[Index] = (UCHAR)(0xA0 + Number * 16 + Index);

    Interval.QuadPart = -10 * 10000;
    Status = STATUS_UNSUCCESSFUL;
    for (Attempt = 0; Attempt < 500; Attempt++)
    {
        Status = WheaReportHwError(&Packet.Header);
        if (Status != STATUS_INSUFFICIENT_RESOURCES)
            break;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    return Status;
}

/* Counts the events of the WHEA logger in the file, per report number, and checks each one against what was reported */
static
VOID
CheckLogFile(
    _In_ PCUNICODE_STRING DosPath,
    _In_ ULONG ErrorSourceId,
    _Out_writes_(REPORT_COUNT) PULONG Found)
{
    UNICODE_STRING NtPath;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PUCHAR Data;
    ULONG Size, BufferSize, Offset, Pos, Number, Index;
    NTSTATUS Status;
    WCHAR Name[300];

    RtlZeroMemory(Found, REPORT_COUNT * sizeof(ULONG));
    RtlInitEmptyUnicodeString(&NtPath, Name, sizeof(Name));
    RtlAppendUnicodeToString(&NtPath, L"\\??\\");
    RtlAppendUnicodeStringToString(&NtPath, DosPath);
    InitializeObjectAttributes(&Attributes, &NtPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &Io, FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Size = NT_SUCCESS(Status) ? (ULONG)Info.EndOfFile.QuadPart : 0;
    ok(Size >= sizeof(WMI_BUFFER_HEADER) && Size <= 4 * 1024 * 1024, "log file size %lu\n", Size);
    Data = (Size >= sizeof(WMI_BUFFER_HEADER) && Size <= 4 * 1024 * 1024) ? ExAllocatePoolWithTag(PagedPool, Size, WHEA_ETW_TAG) : NULL;
    if (Data)
    {
        Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Data, Size, NULL, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Data, WHEA_ETW_TAG);
            Data = NULL;
        }
    }

    ZwClose(File);
    if (!Data)
        return;

    BufferSize = ((PWMI_BUFFER_HEADER)Data)->BufferSize;
    ok(BufferSize >= 1024 && Size % BufferSize == 0, "buffer size %lu, file size %lu\n", BufferSize, Size);
    if (BufferSize < 1024 || Size % BufferSize)
    {
        ExFreePoolWithTag(Data, WHEA_ETW_TAG);
        return;
    }

    for (Offset = BufferSize; Offset < Size; Offset += BufferSize)
    {
        PWMI_BUFFER_HEADER Buffer = (PWMI_BUFFER_HEADER)(Data + Offset);
        ULONG Used = min(Buffer->SavedOffset, BufferSize);

        for (Pos = sizeof(WMI_BUFFER_HEADER); Pos + 80 <= Used; )
        {
            PUCHAR Record = (PUCHAR)Buffer + Pos;
            USHORT RecordSize = *(PUSHORT)Record;

            if (RecordSize < 8 || RecordSize == 0xFFFF || RecordSize > Used - Pos)
                break;

            if (Record[3] == 0xC0 && (Record[2] == 0x13 || Record[2] == 0x12) &&
                RtlEqualMemory(Record + 24, &WheaLoggerGuid, sizeof(GUID)))
            {
                PEVENT_DESCRIPTOR Descriptor = (PEVENT_DESCRIPTOR)(Record + 40);
                ULONG PayloadSize = RecordSize - 80;
                PUCHAR Payload = Record + 80;
                PWHEA_ERROR_RECORD_HEADER Header = (PWHEA_ERROR_RECORD_HEADER)(Payload + sizeof(ULONG));
                PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section = (PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR)(Header + 1);
                PTEST_PACKET Packet = (PTEST_PACKET)(Section + 1);
                ULONG Expected = sizeof(*Header) + sizeof(*Section) + sizeof(*Packet);

                ok_eq_ulong(PayloadSize, (ULONG)sizeof(ULONG) + Expected);
                if (PayloadSize != sizeof(ULONG) + Expected)
                {
                    Pos += ALIGN_UP_BY(RecordSize, 8);
                    continue;
                }

                if (Packet->Header.ErrorSourceId != ErrorSourceId)
                {
                    Pos += ALIGN_UP_BY(RecordSize, 8);
                    continue;
                }

                Number = (Packet->Data[0] - 0xA0) / 16;
                ok(Number < REPORT_COUNT, "An event of this source with unknown data 0x%x\n", Packet->Data[0]);
                if (Number >= REPORT_COUNT)
                {
                    Pos += ALIGN_UP_BY(RecordSize, 8);
                    continue;
                }

                Found[Number]++;
                ok_eq_uint(Descriptor->Id, ReportEventId[Number]);
                ok_eq_uint(Descriptor->Level, ReportEventId[Number] + 1);
                ok_eq_uint(Descriptor->Channel, 8);
                ok_eq_uint(Descriptor->Version, 0);
                ok_eq_ulonglong(Descriptor->Keyword, 0x8000000000000002ULL);
                ok_eq_ulong(*(PULONG)Payload, Expected);
                ok(RtlEqualMemory(&Header->Signature, "CPER", 4), "The record of report %lu does not start with CPER\n", Number);
                ok_eq_ulong(Header->Length, Expected);
                ok_eq_uint(Header->SectionCount, 1);
                ok_eq_int(Header->Severity, ReportSeverity[Number]);
                ok_eq_ulong(Section->SectionOffset, (ULONG)(sizeof(*Header) + sizeof(*Section)));
                ok_eq_ulong(Section->SectionLength, (ULONG)sizeof(*Packet));
                ok(RtlEqualMemory(&Section->SectionType, &PacketSectionGuid, sizeof(GUID)), "Report %lu: another section type\n", Number);
                ok_eq_hex(Packet->Header.Signature, WHEA_ERROR_PACKET_V2_SIGNATURE);
                ok_eq_int(Packet->Header.ErrorSeverity, ReportSeverity[Number]);
                for (Index = 0; Index < sizeof(Packet->Data); Index++)
                {
                    if (Packet->Data[Index] != (UCHAR)(0xA0 + Number * 16 + Index))
                        break;
                }

                ok_eq_ulong(Index, (ULONG)sizeof(Packet->Data));
            }

            Pos += ALIGN_UP_BY(RecordSize, 8);
        }
    }

    ExFreePoolWithTag(Data, WHEA_ETW_TAG);
}

START_TEST(WheaEtwLog)
{
    WMI_LOGGER_INFORMATION Info;
    WCHAR SessionName[] = L"KmtWheaEtw";
    WCHAR FileName[200];
    UNICODE_STRING LogFile;
    ETW_ENABLE_PARAMETERS Params;
    TEST_DESCRIPTOR Descriptor;
    LARGE_INTEGER Interval;
    ULONG Found[REPORT_COUNT];
    ULONG LoggerId, ReturnSize, Number;
    NTSTATUS Status;

    RtlStringCbPrintfW(FileName, sizeof(FileName), L"%s\\kmtest_wheaetw.etl", SharedUserData->NtSystemRoot);
    RtlInitUnicodeString(&LogFile, FileName);

    RtlZeroMemory(&Info, sizeof(Info));
    Info.Wnode.BufferSize = sizeof(Info);
    Info.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    Info.Wnode.ClientContext = EVENT_TRACE_CLOCK_PERFCOUNTER;
    Info.BufferSize = 4;
    Info.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    RtlInitUnicodeString(&Info.LoggerName, SessionName);
    Info.LogFileName = LogFile;
    Status = WmiStartTrace(&Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    LoggerId = (ULONG)Info.Wnode.HistoricalContext;

    RtlZeroMemory(&Params, sizeof(Params));
    Params.ProviderId = WheaLoggerGuid;
    Params.LoggerId = LoggerId;
    Params.ControlCode = EVENT_CONTROL_CODE_ENABLE_PROVIDER;
    Params.Level = TRACE_LEVEL_INFORMATION;
    Status = ZwTraceControl(EtwEnableGuidCode, &Params, sizeof(Params), NULL, 0, &ReturnSize);
    ok_eq_hex(Status, STATUS_SUCCESS);

    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Descriptor.Length = sizeof(Descriptor);
    Descriptor.Version = WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10;
    Descriptor.Type = WheaErrSrcTypeGeneric;
    Descriptor.State = WheaErrSrcStateStarted;
    Descriptor.MaxRawDataLength = sizeof(((PTEST_PACKET)0)->Data);
    Descriptor.NumRecordsToPreallocate = 1;
    Descriptor.MaxSectionsPerRecord = 1;
    Status = WheaAddErrorSource((PWHEA_ERROR_SOURCE_DESCRIPTOR)&Descriptor, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* The worker handles one report at a time, so once report N + 2 is accepted report N is logged completely */
        for (Number = 0; Number < REPORT_COUNT; Number++)
            ok_eq_hex(ReportWhenTaken(Descriptor.ErrorSourceId, Number), STATUS_SUCCESS);

        Interval.QuadPart = -100 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        WheaRemoveErrorSource(Descriptor.ErrorSourceId);
    }

    Info.Wnode.BufferSize = sizeof(Info);
    Info.Wnode.HistoricalContext = LoggerId;
    RtlInitUnicodeString(&Info.LoggerName, SessionName);
    Status = WmiStopTrace(&Info);
    ok_eq_hex(Status, STATUS_SUCCESS);

    CheckLogFile(&LogFile, Descriptor.ErrorSourceId, Found);
    for (Number = 0; Number < CHECKED_REPORTS; Number++)
        ok_eq_ulong(Found[Number], 1UL);
    for (Number = CHECKED_REPORTS; Number < REPORT_COUNT; Number++)
        ok(Found[Number] <= 1, "Report %lu was logged %lu times\n", Number, Found[Number]);
}
