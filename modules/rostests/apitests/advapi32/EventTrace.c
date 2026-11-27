/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A user-mode provider into a file session: StartTrace,
 *              EventRegister, EnableTraceEx2, EventEnabled, EventWrite,
 *              ControlTrace stop, then the .etl read back
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * The file layout is the one Windows writes: a header buffer, then event
 * buffers whose records are EVENT_HEADERs (marker 0xC013 or 0xC012).
 */

#include "precomp.h"

#include <wmistr.h>
#include <evntrace.h>
#include <evntprov.h>
#include <strsafe.h>

#define SESSION_NAME L"LiberNT EventTrace test"

static const GUID TestProvider = { 0x2f9b6c11, 0x8d3e, 0x4a75, { 0xb1, 0x40, 0x6e, 0x27, 0x93, 0x5c, 0x0a, 0xd8 } };

/* the enable notification the session sends, recorded for the diagnosis */
static volatile ULONG CallbackSeen;
static volatile ULONG CallbackIsEnabled;
static volatile UCHAR CallbackLevel;
static volatile ULONGLONG CallbackMatchAny;

static VOID WINAPI
TestEnableCallback(
    _In_ LPCGUID SourceId,
    _In_ ULONG IsEnabled,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData,
    _In_opt_ PVOID CallbackContext)
{
    (void)SourceId; (void)MatchAllKeyword; (void)FilterData; (void)CallbackContext;
    CallbackSeen++;
    CallbackIsEnabled = IsEnabled;
    CallbackLevel = Level;
    CallbackMatchAny = MatchAnyKeyword;
}

static void
ResetProperties(
    _Out_ PEVENT_TRACE_PROPERTIES Properties,
    _In_ ULONG Size,
    _In_ PCWSTR Path)
{
    ZeroMemory(Properties, Size);
    Properties->Wnode.BufferSize = Size;
    Properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    Properties->Wnode.ClientContext = 1;
    Properties->BufferSize = 4;
    Properties->MinimumBuffers = 2;
    Properties->MaximumBuffers = 4;
    Properties->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    Properties->LoggerNameOffset = sizeof(*Properties);
    Properties->LogFileNameOffset = sizeof(*Properties) + MAX_PATH * sizeof(WCHAR);
    StringCchCopyW((PWSTR)((PUCHAR)Properties + Properties->LogFileNameOffset), MAX_PATH, Path);
}

static void
Describe(
    _Out_ PEVENT_DESCRIPTOR Descriptor,
    _In_ USHORT Id,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    ZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->Id = Id;
    Descriptor->Level = Level;
    Descriptor->Opcode = 1;
    Descriptor->Task = 9;
    Descriptor->Keyword = Keyword;
}

/* Our provider's records in the file, each checked */
static ULONG
CountRecords(
    _In_ PCWSTR Path)
{
    HANDLE File;
    DWORD Size, Read, BufferSize, Offset, Pos, Found = 0;
    PUCHAR Data;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "cannot open the log file: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return 0;
    Size = GetFileSize(File, NULL);
    ok(Size >= 0x48 && Size <= 4 * 1024 * 1024, "log file size %lu\n", Size);
    Data = (Size >= 0x48 && Size <= 4 * 1024 * 1024) ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    if (Data)
    {
        ok(ReadFile(File, Data, Size, &Read, NULL) && Read == Size, "ReadFile failed: %lu\n", GetLastError());
    }
    CloseHandle(File);
    if (!Data)
        return 0;

    BufferSize = *(PULONG)Data;
    ok_int(*(PUSHORT)(Data + 0x36), 4);   /* the header buffer */
    ok(BufferSize >= 1024 && Size % BufferSize == 0, "buffer size %lu, file size %lu\n", BufferSize, Size);
    if (BufferSize < 1024 || Size % BufferSize)
    {
        HeapFree(GetProcessHeap(), 0, Data);
        return 0;
    }

    for (Offset = BufferSize; Offset < Size; Offset += BufferSize)
    {
        PUCHAR Buffer = Data + Offset;
        DWORD Used = min(*(PULONG)(Buffer + 4), BufferSize);   /* SavedOffset */

        for (Pos = 0x48; Pos + 80 <= Used; )
        {
            PUCHAR Record = Buffer + Pos;
            USHORT RecordSize = *(PUSHORT)Record;

            if (RecordSize < 8 || RecordSize == 0xFFFF)
                break;
            if (Record[3] == 0xC0 && (Record[2] == 0x13 || Record[2] == 0x12) &&
                !memcmp(Record + 24, &TestProvider, sizeof(GUID)))
            {
                PEVENT_DESCRIPTOR Descriptor = (PEVENT_DESCRIPTOR)(Record + 40);

                /* the bad-pointer event may be dropped or written; either is fine */
                if (Descriptor->Id != 25)
                {
                    ok(Descriptor->Id >= 20 && Descriptor->Id <= 22, "unexpected event id %u\n", Descriptor->Id);
                    ok_int(Descriptor->Task, 9);
                    ok_int(*(PULONG)(Record + 12), (int)GetCurrentProcessId());
                    ok_int(RecordSize, 80 + sizeof(ULONG) + sizeof(L"user"));
                    ok_hex(*(PULONG)(Record + 80), 0x55660000 + Descriptor->Id);
                    ok(!memcmp(Record + 84, L"user", sizeof(L"user")), "payload string differs\n");
                    Found++;
                }
            }
            Pos += (RecordSize + 7) & ~7;
        }
    }
    HeapFree(GetProcessHeap(), 0, Data);
    return Found;
}

static BOOL
WaitProviderEnabled(IN REGHANDLE Reg)
{
    EVENT_DESCRIPTOR Descriptor;
    ULONG i;

    Describe(&Descriptor, 20, TRACE_LEVEL_INFORMATION, 0x2);
    for (i = 0; i < 50; i++)
    {
        if (EventEnabled(Reg, &Descriptor))
            return TRUE;
        Sleep(100);
    }
    return FALSE;
}

START_TEST(EventTrace)
{
    PEVENT_TRACE_PROPERTIES Properties;
    WCHAR Path[MAX_PATH];
    TRACEHANDLE Session = 0;
    REGHANDLE Reg = 0;
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data[2];
    ULONG Size, Status, Value;
    ULONG StartStatus, EnableStatus, RegStatus;
    BOOL Visible;
    USHORT Id;

    if (!GetTempPathW(ARRAYSIZE(Path), Path) || FAILED(StringCchCatW(Path, ARRAYSIZE(Path), L"eventtrace.etl")))
    {
        skip("No temporary path\n");
        return;
    }
    Size = sizeof(*Properties) + 2 * MAX_PATH * sizeof(WCHAR);
    Properties = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Properties)
        return;

    ResetProperties(Properties, Size, Path);
    ControlTraceW(0, SESSION_NAME, Properties, EVENT_TRACE_CONTROL_STOP);
    ResetProperties(Properties, Size, Path);
    StartStatus = StartTraceW(&Session, SESSION_NAME, Properties);
    if (StartStatus == ERROR_ACCESS_DENIED)
    {
        skip("Starting a trace session needs administrator rights\n");
        HeapFree(GetProcessHeap(), 0, Properties);
        return;
    }
    ok_long(StartStatus, ERROR_SUCCESS);
    if (StartStatus != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, Properties);
        return;
    }

    RegStatus = EventRegister(&TestProvider, TestEnableCallback, NULL, &Reg);
    ok_long(RegStatus, ERROR_SUCCESS);

    /* Nothing is enabled yet */
    Describe(&Descriptor, 20, TRACE_LEVEL_INFORMATION, 0x2);
    ok(!EventEnabled(Reg, &Descriptor), "EventEnabled before the enable\n");

    EnableStatus = EnableTraceEx2(Session, &TestProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                                  0x2, 0, 0, NULL);
    ok_long(EnableStatus, ERROR_SUCCESS);

    /* the enable is synchronous on Orosi and logman enables providers on the
       same Win11 VM, so an invisible enable here is a failure to diagnose,
       not a state to skip: report every status and the callback values */
    Visible = WaitProviderEnabled(Reg);
    ok(Visible, "enable never became visible: StartTrace=%lu EnableTraceEx2=%lu register=%lu callback(seen=%lu isenabled=%lu level=%u matchany=%I64x)\n",
       StartStatus, EnableStatus, RegStatus, CallbackSeen, CallbackIsEnabled, CallbackLevel, CallbackMatchAny);
    if (!Visible)
        skip("session enable did not become visible; see the failed row above\n");
    if (Visible)
    {
        Describe(&Descriptor, 20, TRACE_LEVEL_INFORMATION, 0x2);
        ok(EventEnabled(Reg, &Descriptor), "EventEnabled(level 4, keyword 2)\n");
        Describe(&Descriptor, 20, TRACE_LEVEL_VERBOSE, 0x2);
        ok(!EventEnabled(Reg, &Descriptor), "EventEnabled(level 5)\n");
        Describe(&Descriptor, 20, TRACE_LEVEL_INFORMATION, 0x4);
        ok(!EventEnabled(Reg, &Descriptor), "EventEnabled(keyword 4)\n");
        ok(EventProviderEnabled(Reg, TRACE_LEVEL_ERROR, 0), "EventProviderEnabled(level 2, any keyword)\n");

        /* Three that match, two that are filtered out */
        for (Id = 20; Id <= 22; Id++)
        {
            Value = 0x55660000 + Id;
            EventDataDescCreate(&Data[0], &Value, sizeof(Value));
            EventDataDescCreate(&Data[1], L"user", sizeof(L"user"));
            Describe(&Descriptor, Id, TRACE_LEVEL_INFORMATION, 0x2);
            ok_long(EventWrite(Reg, &Descriptor, 2, Data), ERROR_SUCCESS);
        }
        Describe(&Descriptor, 23, TRACE_LEVEL_VERBOSE, 0x2);
        ok_long(EventWrite(Reg, &Descriptor, 2, Data), ERROR_SUCCESS);
        Describe(&Descriptor, 24, TRACE_LEVEL_INFORMATION, 0x4);
        ok_long(EventWrite(Reg, &Descriptor, 2, Data), ERROR_SUCCESS);

        /* Win11 does not validate payload pointers at write time; the write
           succeeds and the event is dropped or captured later */
        Data[1].Ptr = (ULONGLONG)(ULONG_PTR)0x10;
        Describe(&Descriptor, 25, TRACE_LEVEL_INFORMATION, 0x2);
        Status = EventWrite(Reg, &Descriptor, 2, Data);
        /* not yet exercised with the provider enabled: a Win11 error is
           reported as broken, Orosi must return success; the printed code
           is what the enabled-provider probe reads */
        ok(Status == ERROR_SUCCESS || broken(Status == ERROR_NOACCESS),
           "EventWrite with a bad data pointer returned %lu\n", Status);
    }

    ResetProperties(Properties, Size, Path);
    Status = ControlTraceW(Session, NULL, Properties, EVENT_TRACE_CONTROL_STOP);
    ok_long(Status, ERROR_SUCCESS);

    if (Visible)
    {
        /* The stopped session no longer enables the provider */
        Describe(&Descriptor, 20, TRACE_LEVEL_INFORMATION, 0x2);
        ok(!EventEnabled(Reg, &Descriptor), "EventEnabled after the session stopped\n");
        Sleep(500);
        ok_long(CountRecords(Path), 3);
    }
    ok_long(EventUnregister(Reg), ERROR_SUCCESS);
    DeleteFileW(Path);
    HeapFree(GetProcessHeap(), 0, Properties);
}
