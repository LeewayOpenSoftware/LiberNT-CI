/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel ETW providers: enable callbacks, enabled state, delivery
 *              to a file session, and the .etl the session writes
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>
#define _WMIKM_
#include <ntwmi.h>

NTSTATUS NTAPI WmiStartTrace(_Inout_ PWMI_LOGGER_INFORMATION LoggerInformation);
NTSTATUS NTAPI WmiStopTrace(_Inout_ PWMI_LOGGER_INFORMATION LoggerInformation);

static GUID KmtEtwCoreGuid = { 0x7a3c2e51, 0x4b1d, 0x4f60, { 0x9e, 0x22, 0x31, 0x5a, 0x8c, 0x0d, 0x47, 0xb6 } };

#define KMT_ETW_TAG 'cwtE'

typedef struct _KMT_CALLBACKS
{
    LONG Count;
    ULONG LastCode;
    UCHAR LastLevel;
    ULONGLONG LastMatchAny;
} KMT_CALLBACKS, *PKMT_CALLBACKS;

static
VOID
NTAPI
KmtEnableCallback(
    _In_ LPCGUID SourceId,
    _In_ ULONG ControlCode,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData,
    _Inout_opt_ PVOID CallbackContext)
{
    PKMT_CALLBACKS Callbacks = CallbackContext;

    UNREFERENCED_PARAMETER(SourceId);
    UNREFERENCED_PARAMETER(MatchAllKeyword);
    UNREFERENCED_PARAMETER(FilterData);

    ok_irql(PASSIVE_LEVEL);
    InterlockedIncrement(&Callbacks->Count);
    Callbacks->LastCode = ControlCode;
    Callbacks->LastLevel = Level;
    Callbacks->LastMatchAny = MatchAnyKeyword;
}

static
NTSTATUS
KmtEnable(
    _In_ ULONG LoggerId,
    _In_ UCHAR ControlCode,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAny)
{
    ETW_ENABLE_PARAMETERS Params;
    ULONG ReturnSize;

    RtlZeroMemory(&Params, sizeof(Params));
    Params.ProviderId = KmtEtwCoreGuid;
    Params.LoggerId = LoggerId;
    Params.ControlCode = ControlCode;
    Params.Level = Level;
    Params.MatchAnyKeyword = MatchAny;
    return ZwTraceControl(EtwEnableGuidCode, &Params, sizeof(Params), NULL, 0, &ReturnSize);
}

static
VOID
KmtDescriptor(
    _Out_ PEVENT_DESCRIPTOR Descriptor,
    _In_ USHORT Id,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->Id = Id;
    Descriptor->Level = Level;
    Descriptor->Opcode = 1;
    Descriptor->Task = 7;
    Descriptor->Keyword = Keyword;
}

/* Counts the records of our provider in the file and checks their header and payload */
static
ULONG
KmtCheckLogFile(
    _In_ PCUNICODE_STRING DosPath)
{
    UNICODE_STRING NtPath;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PUCHAR Data;
    ULONG Size, BufferSize, Offset, Pos, Found = 0;
    NTSTATUS Status;
    WCHAR Name[300];

    RtlInitEmptyUnicodeString(&NtPath, Name, sizeof(Name));
    RtlAppendUnicodeToString(&NtPath, L"\\??\\");
    RtlAppendUnicodeStringToString(&NtPath, DosPath);
    InitializeObjectAttributes(&Attributes, &NtPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenFile(&File, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &Io, FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return 0;

    Status = ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Size = (ULONG)Info.EndOfFile.QuadPart;
    ok(Size >= sizeof(WMI_BUFFER_HEADER) && Size <= 4 * 1024 * 1024, "log file size %lu\n", Size);
    Data = (Size >= sizeof(WMI_BUFFER_HEADER) && Size <= 4 * 1024 * 1024) ? ExAllocatePoolWithTag(PagedPool, Size, KMT_ETW_TAG) : NULL;
    if (Data)
    {
        Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Data, Size, NULL, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(File);
    if (!Data)
        return 0;

    /* The first buffer is the header buffer; then event buffers of the same size */
    BufferSize = ((PWMI_BUFFER_HEADER)Data)->BufferSize;
    ok_eq_uint(((PWMI_BUFFER_HEADER)Data)->BufferType, ETW_BUFFER_TYPE_HEADER);
    ok(BufferSize >= 1024 && Size % BufferSize == 0, "buffer size %lu, file size %lu\n", BufferSize, Size);
    if (BufferSize < 1024 || Size % BufferSize)
    {
        ExFreePoolWithTag(Data, KMT_ETW_TAG);
        return 0;
    }

    for (Offset = BufferSize; Offset < Size; Offset += BufferSize)
    {
        PWMI_BUFFER_HEADER Buffer = (PWMI_BUFFER_HEADER)(Data + Offset);
        ULONG Used = min(Buffer->SavedOffset, BufferSize);

        for (Pos = sizeof(WMI_BUFFER_HEADER); Pos + 80 <= Used; )
        {
            PUCHAR Record = (PUCHAR)Buffer + Pos;
            USHORT RecordSize = *(PUSHORT)Record;

            if (RecordSize < 8 || RecordSize == 0xFFFF)
                break;
            if (Record[3] == 0xC0 && (Record[2] == 0x13 || Record[2] == 0x12) &&
                RtlEqualMemory(Record + 24, &KmtEtwCoreGuid, sizeof(GUID)))
            {
                PEVENT_DESCRIPTOR Descriptor = (PEVENT_DESCRIPTOR)(Record + 40);
                ok(Descriptor->Id == 10 || Descriptor->Id == 11 || Descriptor->Id == 12, "unexpected event id %u\n", Descriptor->Id);
                ok_eq_uint(Descriptor->Task, 7);
                ok_eq_uint(RecordSize, 80 + sizeof(ULONG) + sizeof(L"etw"));
                ok_eq_hex(*(PULONG)(Record + 80), 0x11220000UL + Descriptor->Id);
                ok(RtlEqualMemory(Record + 84, L"etw", sizeof(L"etw")), "payload string differs\n");
                Found++;
            }
            Pos += ALIGN_UP_BY(RecordSize, 8);
        }
    }
    ExFreePoolWithTag(Data, KMT_ETW_TAG);
    return Found;
}

START_TEST(EtwProviderKM)
{
    KMT_CALLBACKS Callbacks = { 0 };
    WMI_LOGGER_INFORMATION Info;
    WCHAR SessionName[] = L"KmtEtwCore";
    WCHAR FileName[200];
    UNICODE_STRING LogFile;
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data[2];
    REGHANDLE RegHandle = 0;
    ULONG LoggerId, Value, Found;
    USHORT Id;
    NTSTATUS Status;

    /* A file next to the system root: a DOS path, as a controller passes it */
    RtlStringCbPrintfW(FileName, sizeof(FileName), L"%s\\kmtest_etwcore.etl", SharedUserData->NtSystemRoot);
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

    /* Enabled before the provider registers: the registration is told at once */
    Status = KmtEnable(LoggerId, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0x2);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = EtwRegister(&KmtEtwCoreGuid, KmtEnableCallback, &Callbacks, &RegHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_long(Callbacks.Count, 1L);
    ok_eq_ulong(Callbacks.LastCode, (ULONG)EVENT_CONTROL_CODE_ENABLE_PROVIDER);
    ok_eq_uint(Callbacks.LastLevel, TRACE_LEVEL_INFORMATION);
    ok_eq_ulonglong(Callbacks.LastMatchAny, 0x2ULL);

    /* Level and keyword filtering */
    KmtDescriptor(&Descriptor, 10, TRACE_LEVEL_INFORMATION, 0x2);
    ok_bool_true(EtwEventEnabled(RegHandle, &Descriptor), "EtwEventEnabled(level 4, keyword 2):");
    KmtDescriptor(&Descriptor, 10, TRACE_LEVEL_VERBOSE, 0x2);
    ok_bool_false(EtwEventEnabled(RegHandle, &Descriptor), "EtwEventEnabled(level 5):");
    KmtDescriptor(&Descriptor, 10, TRACE_LEVEL_INFORMATION, 0x4);
    ok_bool_false(EtwEventEnabled(RegHandle, &Descriptor), "EtwEventEnabled(keyword 4):");
    ok_bool_true(EtwProviderEnabled(RegHandle, TRACE_LEVEL_ERROR, 0), "EtwProviderEnabled(level 2, any):");

    /* Three events that match, two that the session filters out */
    for (Id = 10; Id <= 12; Id++)
    {
        Value = 0x11220000 + Id;
        EventDataDescCreate(&Data[0], &Value, sizeof(Value));
        EventDataDescCreate(&Data[1], L"etw", sizeof(L"etw"));
        KmtDescriptor(&Descriptor, Id, TRACE_LEVEL_INFORMATION, 0x2);
        Status = EtwWrite(RegHandle, &Descriptor, NULL, 2, Data);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    KmtDescriptor(&Descriptor, 13, TRACE_LEVEL_VERBOSE, 0x2);
    Status = EtwWrite(RegHandle, &Descriptor, NULL, 2, Data);
    ok_eq_hex(Status, STATUS_SUCCESS);
    KmtDescriptor(&Descriptor, 14, TRACE_LEVEL_INFORMATION, 0x4);
    Status = EtwWrite(RegHandle, &Descriptor, NULL, 2, Data);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Stopping the session disables the provider, with a callback */
    Info.Wnode.BufferSize = sizeof(Info);
    Info.Wnode.HistoricalContext = LoggerId;
    RtlInitUnicodeString(&Info.LoggerName, SessionName);
    Status = WmiStopTrace(&Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_long(Callbacks.Count, 2L);
    ok_eq_ulong(Callbacks.LastCode, (ULONG)EVENT_CONTROL_CODE_DISABLE_PROVIDER);
    KmtDescriptor(&Descriptor, 10, TRACE_LEVEL_INFORMATION, 0x2);
    ok_bool_false(EtwEventEnabled(RegHandle, &Descriptor), "EtwEventEnabled after stop:");

    /* Enabling a stopped session fails */
    Status = KmtEnable(LoggerId, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0);
    ok_eq_hex(Status, STATUS_WMI_INSTANCE_NOT_FOUND);

    Status = EtwUnregister(RegHandle);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Found = KmtCheckLogFile(&LogFile);
    ok_eq_ulong(Found, 3UL);
}
