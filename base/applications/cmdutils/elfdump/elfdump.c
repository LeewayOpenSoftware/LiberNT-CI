/*
 * PROJECT:     LiberNT Event Log dump tool
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Read a classic event log with OpenEventLog/ReadEventLog and print
 *              every record: number, time, type, event ID, source, computer,
 *              insertion strings and raw data. Used to check what the Event Log
 *              service really stored, independently of any viewer.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static const WCHAR *
TypeName(WORD Type)
{
    switch (Type)
    {
        case EVENTLOG_SUCCESS:          return L"Success";
        case EVENTLOG_ERROR_TYPE:       return L"Error";
        case EVENTLOG_WARNING_TYPE:     return L"Warning";
        case EVENTLOG_INFORMATION_TYPE: return L"Information";
        case EVENTLOG_AUDIT_SUCCESS:    return L"AuditSuccess";
        case EVENTLOG_AUDIT_FAILURE:    return L"AuditFailure";
    }
    return L"Unknown";
}

static void
PrintRecord(const EVENTLOGRECORD *Rec)
{
    const BYTE *Base = (const BYTE *)Rec;
    const WCHAR *Source = (const WCHAR *)(Base + sizeof(EVENTLOGRECORD));
    const WCHAR *Computer = Source + wcslen(Source) + 1;
    const WCHAR *Str = (const WCHAR *)(Base + Rec->StringOffset);
    FILETIME Ft;
    SYSTEMTIME St;
    ULONGLONG T;
    WORD i;
    DWORD d;

    /* seconds since 1970 -> FILETIME (100 ns since 1601) */
    T = ((ULONGLONG)Rec->TimeGenerated + 11644473600ULL) * 10000000ULL;
    Ft.dwLowDateTime = (DWORD)T;
    Ft.dwHighDateTime = (DWORD)(T >> 32);
    FileTimeToSystemTime(&Ft, &St);

    wprintf(L"Record %lu  %04u-%02u-%02u %02u:%02u:%02u UTC  %ls  EventID=%lu (0x%08lx)  Category=%u\n",
            Rec->RecordNumber, St.wYear, St.wMonth, St.wDay, St.wHour, St.wMinute, St.wSecond,
            TypeName(Rec->EventType), Rec->EventID & 0xFFFF, Rec->EventID, Rec->EventCategory);
    wprintf(L"  Source=%ls  Computer=%ls\n", Source, Computer);
    for (i = 0; i < Rec->NumStrings; i++)
    {
        wprintf(L"  String[%u]=%ls\n", i, Str);
        Str += wcslen(Str) + 1;
    }
    if (Rec->DataLength)
    {
        const BYTE *Data = Base + Rec->DataOffset;
        wprintf(L"  Data(%lu):", Rec->DataLength);
        for (d = 0; d < Rec->DataLength && d < 64; d++)
            wprintf(L" %02x", Data[d]);
        wprintf(Rec->DataLength > 64 ? L" ...\n" : L"\n");
    }
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *LogName = (argc > 1) ? argv[1] : L"System";
    DWORD Max = (argc > 2) ? (DWORD)_wtoi(argv[2]) : 0xFFFFFFFF;
    DWORD Count = 0, Total = 0, Read, Needed, Size = 0x10000;
    BYTE *Buf;
    HANDLE h;

    h = OpenEventLogW(NULL, LogName);
    if (!h)
    {
        wprintf(L"OpenEventLog(%ls) failed: %lu\n", LogName, GetLastError());
        return 1;
    }
    GetNumberOfEventLogRecords(h, &Total);
    wprintf(L"Log %ls: %lu records\n", LogName, Total);

    Buf = malloc(Size);
    if (!Buf)
        return 2;

    /* newest first, like a viewer */
    while (Count < Max &&
           ReadEventLogW(h, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, Buf, Size, &Read, &Needed))
    {
        DWORD Off = 0;
        while (Off < Read && Count < Max)
        {
            const EVENTLOGRECORD *Rec = (const EVENTLOGRECORD *)(Buf + Off);
            if (Rec->Length == 0 || Off + Rec->Length > Read)
                break;
            PrintRecord(Rec);
            Off += Rec->Length;
            Count++;
        }
    }
    if (GetLastError() != ERROR_HANDLE_EOF && Count < Max && Count < Total)
        wprintf(L"ReadEventLog stopped: %lu\n", GetLastError());

    wprintf(L"Printed %lu records\n", Count);
    free(Buf);
    CloseEventLog(h);
    return 0;
}
