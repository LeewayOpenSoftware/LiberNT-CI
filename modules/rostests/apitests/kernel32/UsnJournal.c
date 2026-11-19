/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USN change journal FSCTLs on an NTFS volume
 */

#include "precomp.h"

#include <winioctl.h>

#define USN_TEST_FILE_NAME  L"usntest.txt"
#define USN_CHILD_MARKER    L"UsnJournalChild:"

static WCHAR VolumePath[] = L"\\\\.\\?:";
static WCHAR FilePath[] = L"?:\\" USN_TEST_FILE_NAME;

/* The first NTFS volume after C: (the lab attaches a second disk formatted with mkntfs) */
static WCHAR FindNtfsVolume(VOID)
{
    WCHAR Root[] = L"?:\\";
    WCHAR FsName[32];
    WCHAR Letter;

    for (Letter = L'D'; Letter <= L'Z'; Letter++)
    {
        Root[0] = Letter;
        if (GetDriveTypeW(Root) != DRIVE_FIXED)
            continue;
        if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, FsName, ARRAYSIZE(FsName)) &&
            !_wcsicmp(FsName, L"NTFS"))
        {
            return Letter;
        }
    }
    return 0;
}

static HANDLE OpenVolume(DWORD Access, DWORD Flags)
{
    return CreateFileW(VolumePath, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, Flags, NULL);
}

/* The first version 2 record named USN_TEST_FILE_NAME in a READ/ENUM output (after the 8-byte header) */
static PUSN_RECORD_V2 FindTestRecord(PUCHAR Buffer, DWORD Returned, USN Usn)
{
    DWORD Offset = sizeof(USN);

    while (Offset + FIELD_OFFSET(USN_RECORD_V2, FileName) <= Returned)
    {
        PUSN_RECORD_V2 Record = (PUSN_RECORD_V2)(Buffer + Offset);

        if (Record->RecordLength < FIELD_OFFSET(USN_RECORD_V2, FileName) || Offset + Record->RecordLength > Returned)
            break;
        if (Record->MajorVersion == 2 &&
            Record->FileNameLength == sizeof(USN_TEST_FILE_NAME) - sizeof(WCHAR) &&
            !memcmp((PUCHAR)Record + Record->FileNameOffset, USN_TEST_FILE_NAME, Record->FileNameLength) &&
            (Usn < 0 || Record->Usn == Usn))
        {
            return Record;
        }
        Offset += Record->RecordLength;
    }
    return NULL;
}

static BOOL WriteCloseRecord(HANDLE File, USN *Usn)
{
    DWORD Returned = 0;

    *Usn = -1;
    return DeviceIoControl(File, FSCTL_WRITE_USN_CLOSE_RECORD, NULL, 0, Usn, sizeof(*Usn), &Returned, NULL) &&
           Returned == sizeof(*Usn);
}

typedef struct _TIMED_READ
{
    HANDLE Volume;
    READ_USN_JOURNAL_DATA_V0 Input;
    UCHAR Buffer[4096];
    DWORD Returned;
    BOOL Result;
    DWORD Error;
    DWORD Elapsed;
} TIMED_READ;

static DWORD WINAPI TimedReadThread(PVOID Context)
{
    TIMED_READ *Read = Context;
    DWORD Start = GetTickCount();

    Read->Result = DeviceIoControl(Read->Volume, FSCTL_READ_USN_JOURNAL, &Read->Input, sizeof(Read->Input),
                                   Read->Buffer, sizeof(Read->Buffer), &Read->Returned, NULL);
    Read->Error = GetLastError();
    Read->Elapsed = GetTickCount() - Start;
    return 0;
}

/* Child: a token without Administrators must not open the volume for data access */
static int RunChild(LPCWSTR Marker)
{
    HANDLE Volume;
    USN_JOURNAL_DATA_V0 Data;
    DWORD Returned;

    VolumePath[4] = Marker[wcslen(USN_CHILD_MARKER)];
    Volume = OpenVolume(GENERIC_READ, 0);
    if (Volume != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Volume);
        return 1;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED)
        return 2;

    /* A volume handle without data access: the journal FSCTLs are an invalid function */
    Volume = OpenVolume(0, 0);
    if (Volume == INVALID_HANDLE_VALUE)
        return 0;
    if (DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL))
    {
        CloseHandle(Volume);
        return 3;
    }
    Returned = GetLastError();
    CloseHandle(Volume);
    return (Returned == ERROR_INVALID_FUNCTION) ? 0 : 4;
}

static void TestUnprivilegedOpen(WCHAR Letter)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    SID_AND_ATTRIBUTES Disable = { 0 };
    PROCESS_INFORMATION Pi = { 0 };
    STARTUPINFOW Si = { sizeof(Si) };
    HANDLE Token = NULL, Restricted = NULL;
    WCHAR Exe[MAX_PATH], CommandLine[MAX_PATH + 64];
    DWORD ExitCode = 0xFFFFFFFF;

    if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &Disable.Sid) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &Token) ||
        !CreateRestrictedToken(Token, 0, 1, &Disable, 0, NULL, 0, NULL, &Restricted))
    {
        skip("Cannot build a token without Administrators (error %lu)\n", GetLastError());
        goto Done;
    }
    GetModuleFileNameW(NULL, Exe, ARRAYSIZE(Exe));
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%ls\" UsnJournal %ls%lc", Exe, USN_CHILD_MARKER, Letter);
    if (!CreateProcessAsUserW(Restricted, Exe, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi))
    {
        skip("CreateProcessAsUserW failed with %lu\n", GetLastError());
        goto Done;
    }
    ok(WaitForSingleObject(Pi.hProcess, 30000) == WAIT_OBJECT_0, "child did not finish\n");
    GetExitCodeProcess(Pi.hProcess, &ExitCode);
    ok(ExitCode == 0, "unprivileged volume access: child exit %lu (1 opened for read, 2 other open error, "
                      "3/4 FSCTL on a no-access handle not refused as invalid function)\n", ExitCode);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);

Done:
    if (Restricted)
        CloseHandle(Restricted);
    if (Token)
        CloseHandle(Token);
    if (Disable.Sid)
        FreeSid(Disable.Sid);
}

/* The wait edges: Timeout with nothing, handle close and dismount with a read still waiting */
static void TestWaitEdges(void)
{
    USN_JOURNAL_DATA_V1 Data;
    READ_USN_JOURNAL_DATA_V0 Read = { 0 };
    OVERLAPPED Ov = { 0 };
    UCHAR Buffer[512];
    TIMED_READ *Timed;
    HANDLE Volume, Async, Thread;
    DWORD Returned;
    BOOL Result;

    Volume = OpenVolume(GENERIC_READ | GENERIC_WRITE, 0);
    if (Volume == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL))
    {
        skip("No journal for the wait edges (%lu)\n", GetLastError());
        if (Volume != INVALID_HANDLE_VALUE)
            CloseHandle(Volume);
        return;
    }
    Read.ReasonMask = 0xFFFFFFFF;
    Read.UsnJournalID = Data.UsnJournalID;
    Read.StartUsn = Data.NextUsn;

    /* Timeout with no record: returns after the timeout with just the next USN */
    Timed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Timed));
    ok(Timed != NULL, "HeapAlloc failed\n");
    if (Timed)
    {
        Timed->Volume = Volume;
        Timed->Input = Read;
        Timed->Input.BytesToWaitFor = 1024 * 1024;
        Timed->Input.Timeout = 2;
        Thread = CreateThread(NULL, 0, TimedReadThread, Timed, 0, NULL);
        if (WaitForSingleObject(Thread, 15000) == WAIT_OBJECT_0)
        {
            ok(Timed->Result && Timed->Returned >= sizeof(USN) && *(USN *)Timed->Buffer >= Read.StartUsn,
               "no-record timed read: %d (%lu), %lu bytes\n", Timed->Result, Timed->Error, Timed->Returned);
            ok(Timed->Elapsed >= 1500, "no-record timed read returned after %lu ms\n", Timed->Elapsed);
            HeapFree(GetProcessHeap(), 0, Timed);
        }
        else
        {
            /* Still blocked: leave its buffer alone */
            ok(FALSE, "no-record timed read never returned\n");
        }
        CloseHandle(Thread);
    }

    /* Closing the handle of a waiting read cancels it (the cleanup path) */
    Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* Large enough that a stray background record cannot complete the read early */
    Read.BytesToWaitFor = 1024 * 1024;
    Async = OpenVolume(GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
    Result = DeviceIoControl(Async, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), NULL, &Ov);
    ok(!Result && GetLastError() == ERROR_IO_PENDING, "wait before close: %d, %lu\n", Result, GetLastError());
    CloseHandle(Async);
    if (!Result)
    {
        ok(WaitForSingleObject(Ov.hEvent, 10000) == WAIT_OBJECT_0, "close did not end the waiting read\n");
        /* STATUS_CANCELLED */
        ok(Ov.Internal == 0xC0000120, "read ended by close: 0x%lx\n", (ULONG)Ov.Internal);
    }

    /* Dismount with a read waiting: it completes with an error and the system stays up. Last. */
    ResetEvent(Ov.hEvent);
    Async = OpenVolume(GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
    Result = DeviceIoControl(Async, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), NULL, &Ov);
    ok(!Result && GetLastError() == ERROR_IO_PENDING, "wait before dismount: %d, %lu\n", Result, GetLastError());
    DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Returned, NULL);
    ok(DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Returned, NULL),
       "FSCTL_DISMOUNT_VOLUME: %lu\n", GetLastError());
    if (!Result)
    {
        ok(WaitForSingleObject(Ov.hEvent, 10000) == WAIT_OBJECT_0, "dismount did not end the waiting read\n");
        trace("read ended by dismount: 0x%lx\n", (ULONG)Ov.Internal);
        ok(Ov.Internal != STATUS_PENDING && Ov.Internal != 0, "dismounted read status 0x%lx\n", (ULONG)Ov.Internal);
    }
    CloseHandle(Async);
    CloseHandle(Ov.hEvent);
    CloseHandle(Volume);
}

START_TEST(UsnJournal)
{
    LPCWSTR Child = wcsstr(GetCommandLineW(), USN_CHILD_MARKER);
    CREATE_USN_JOURNAL_DATA Create = { 32 * 1024 * 1024, 4 * 1024 * 1024 };
    USN_JOURNAL_DATA_V1 Data = { 0 };
    READ_USN_JOURNAL_DATA_V0 Read = { 0 };
    READ_FILE_USN_DATA Versions = { 2, 2 };
    MFT_ENUM_DATA_V0 Enum = { 0 };
    MARK_HANDLE_INFO Mark = { 0 };
    UCHAR Buffer[16384];
    HANDLE Volume, AsyncVolume, File, Other;
    OVERLAPPED Ov = { 0 };
    PUSN_RECORD_V2 Record;
    USN CloseUsn, SecondUsn, WakeUsn;
    DWORD Returned;
    WCHAR Letter;
    BOOL Result;

    if (Child)
        ExitProcess(RunChild(Child));

    Letter = FindNtfsVolume();
    if (!Letter)
    {
        skip("No NTFS volume after C:\n");
        return;
    }
    VolumePath[4] = Letter;
    FilePath[0] = Letter;
    trace("NTFS volume %lc:\n", Letter);

    Volume = OpenVolume(GENERIC_READ | GENERIC_WRITE, 0);
    if (Volume == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
    {
        /* A standard user: the journal FSCTLs need a volume handle; only the refusal is checked */
        skip("Opening the volume needs administrator rights\n");
        TestUnprivilegedOpen(Letter);
        return;
    }
    ok(Volume != INVALID_HANDLE_VALUE, "open volume: %lu\n", GetLastError());
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    /* Create (or update) the journal, then query it */
    Result = DeviceIoControl(Volume, FSCTL_CREATE_USN_JOURNAL, &Create, sizeof(Create), NULL, 0, &Returned, NULL);
    ok(Result, "FSCTL_CREATE_USN_JOURNAL: %lu\n", GetLastError());
    Result = DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL);
    ok(Result, "FSCTL_QUERY_USN_JOURNAL: %lu\n", GetLastError());
    ok(Returned == sizeof(USN_JOURNAL_DATA_V1), "query returned %lu bytes\n", Returned);
    ok(Data.UsnJournalID != 0, "journal id is 0\n");
    ok(Data.MaxUsn == 0x7FFFFFFFFFFF0000LL, "MaxUsn 0x%I64x\n", Data.MaxUsn);
    ok(Data.MaximumSize == Create.MaximumSize && Data.AllocationDelta == Create.AllocationDelta,
       "sizes %I64u/%I64u\n", Data.MaximumSize, Data.AllocationDelta);
    ok(Data.MinSupportedMajorVersion == 2 && Data.MaxSupportedMajorVersion >= 2, "versions %u..%u\n",
       Data.MinSupportedMajorVersion, Data.MaxSupportedMajorVersion);

    File = CreateFileW(FilePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       CREATE_ALWAYS, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "create %ls: %lu\n", FilePath, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
    {
        CloseHandle(Volume);
        return;
    }

    /* A close record, read back from the journal */
    ok(WriteCloseRecord(File, &CloseUsn), "FSCTL_WRITE_USN_CLOSE_RECORD: %lu\n", GetLastError());
    ok(CloseUsn >= Data.NextUsn, "close USN %I64d before next %I64d\n", CloseUsn, Data.NextUsn);
    Read.StartUsn = 0;
    Read.ReasonMask = 0xFFFFFFFF;
    Read.UsnJournalID = Data.UsnJournalID;
    Result = DeviceIoControl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), &Returned, NULL);
    ok(Result, "FSCTL_READ_USN_JOURNAL: %lu\n", GetLastError());
    Record = Result ? FindTestRecord(Buffer, Returned, CloseUsn) : NULL;
    ok(Record != NULL, "close record not read back (%lu bytes)\n", Returned);
    if (Record)
        ok(Record->Reason & USN_REASON_CLOSE, "reason 0x%lx\n", Record->Reason);

    /* A reason mask that excludes it, and a StartUsn inside a record */
    Read.StartUsn = CloseUsn;
    Read.ReasonMask = USN_REASON_RENAME_NEW_NAME;
    Result = DeviceIoControl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), &Returned, NULL);
    ok(Result && Returned >= sizeof(USN) && !FindTestRecord(Buffer, Returned, CloseUsn),
       "filtered read: %d, %lu bytes\n", Result, Returned);
    Read.StartUsn = CloseUsn + 8;
    Read.ReasonMask = 0xFFFFFFFF;
    Result = DeviceIoControl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), &Returned, NULL);
    ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "read inside a record: %d, %lu\n", Result, GetLastError());
    Read.StartUsn = CloseUsn;
    Read.UsnJournalID = Data.UsnJournalID + 1;
    Result = DeviceIoControl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), &Returned, NULL);
    ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "wrong journal id: %d, %lu\n", Result, GetLastError());
    Read.UsnJournalID = Data.UsnJournalID;

    /* The file's own USN data and the MFT enumeration by USN */
    Result = DeviceIoControl(File, FSCTL_READ_FILE_USN_DATA, &Versions, sizeof(Versions), Buffer + sizeof(USN),
                             sizeof(Buffer) - sizeof(USN), &Returned, NULL);
    ok(Result, "FSCTL_READ_FILE_USN_DATA: %lu\n", GetLastError());
    if (Result)
        ok(FindTestRecord(Buffer, Returned + sizeof(USN), CloseUsn) != NULL, "file USN data does not match\n");
    Enum.StartFileReferenceNumber = 0;
    Enum.LowUsn = CloseUsn;
    Enum.HighUsn = CloseUsn;
    Result = DeviceIoControl(Volume, FSCTL_ENUM_USN_DATA, &Enum, sizeof(Enum), Buffer, sizeof(Buffer), &Returned, NULL);
    ok(Result, "FSCTL_ENUM_USN_DATA: %lu\n", GetLastError());
    if (Result)
        ok(FindTestRecord(Buffer, Returned, CloseUsn) != NULL, "enumeration did not list the file\n");

    /* Source flags through FSCTL_MARK_HANDLE; a file handle is not proof of volume access */
    Mark.UsnSourceInfo = USN_SOURCE_DATA_MANAGEMENT;
    Mark.VolumeHandle = Volume;
    Result = DeviceIoControl(File, FSCTL_MARK_HANDLE, &Mark, sizeof(Mark), NULL, 0, &Returned, NULL);
    ok(Result, "FSCTL_MARK_HANDLE: %lu\n", GetLastError());
    ok(WriteCloseRecord(File, &SecondUsn), "second close record: %lu\n", GetLastError());
    Read.StartUsn = SecondUsn;
    Result = DeviceIoControl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), &Returned, NULL);
    Record = Result ? FindTestRecord(Buffer, Returned, SecondUsn) : NULL;
    ok(Record && Record->SourceInfo == USN_SOURCE_DATA_MANAGEMENT, "source info not stamped\n");
    Other = CreateFileW(FilePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    Mark.VolumeHandle = Other;
    Result = DeviceIoControl(File, FSCTL_MARK_HANDLE, &Mark, sizeof(Mark), NULL, 0, &Returned, NULL);
    ok(!Result && GetLastError() == ERROR_ACCESS_DENIED, "MARK_HANDLE with a file handle: %d, %lu\n", Result, GetLastError());
    CloseHandle(Other);

    /* A read that waits (BytesToWaitFor) is completed by the next record */
    AsyncVolume = OpenVolume(GENERIC_READ | GENERIC_WRITE, FILE_FLAG_OVERLAPPED);
    ok(AsyncVolume != INVALID_HANDLE_VALUE, "open overlapped volume: %lu\n", GetLastError());
    if (AsyncVolume != INVALID_HANDLE_VALUE)
    {
        Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL);
        Read.StartUsn = Data.NextUsn;
        Read.BytesToWaitFor = 1;
        Result = DeviceIoControl(AsyncVolume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), NULL, &Ov);
        ok(!Result && GetLastError() == ERROR_IO_PENDING, "waiting read: %d, %lu\n", Result, GetLastError());
        if (!Result && GetLastError() == ERROR_IO_PENDING)
        {
            ok(WaitForSingleObject(Ov.hEvent, 500) == WAIT_TIMEOUT, "waiting read completed with nothing appended\n");
            ok(WriteCloseRecord(File, &WakeUsn), "waking close record: %lu\n", GetLastError());
            ok(WaitForSingleObject(Ov.hEvent, 10000) == WAIT_OBJECT_0, "waiting read not woken\n");
            Result = GetOverlappedResult(AsyncVolume, &Ov, &Returned, FALSE);
            ok(Result && FindTestRecord(Buffer, Returned, WakeUsn) != NULL, "woken read: %d, %lu bytes\n", Result, Returned);

            /* A waiting read can be cancelled */
            ResetEvent(Ov.hEvent);
            DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL);
            Read.StartUsn = Data.NextUsn;
            Result = DeviceIoControl(AsyncVolume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Buffer, sizeof(Buffer), NULL, &Ov);
            ok(!Result && GetLastError() == ERROR_IO_PENDING, "second waiting read: %d, %lu\n", Result, GetLastError());
            ok(CancelIoEx(AsyncVolume, &Ov), "CancelIoEx: %lu\n", GetLastError());
            ok(WaitForSingleObject(Ov.hEvent, 10000) == WAIT_OBJECT_0, "cancelled read not completed\n");
            Result = GetOverlappedResult(AsyncVolume, &Ov, &Returned, FALSE);
            ok(!Result && GetLastError() == ERROR_OPERATION_ABORTED, "cancelled read: %d, %lu\n", Result, GetLastError());
        }
        CloseHandle(Ov.hEvent);
        CloseHandle(AsyncVolume);
    }

    /* Timeout: records but too few bytes for BytesToWaitFor still return once Timeout elapses */
    {
        TIMED_READ *Timed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Timed));
        HANDLE Thread;
        USN TimedUsn;

        ok(Timed != NULL, "HeapAlloc failed\n");
        if (!Timed)
            goto TimedDone;
        DeviceIoControl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Data, sizeof(Data), &Returned, NULL);
        Timed->Volume = Volume;
        Timed->Input = Read;
        Timed->Input.StartUsn = Data.NextUsn;
        Timed->Input.BytesToWaitFor = 1024 * 1024;
        Timed->Input.Timeout = 2;
        Thread = CreateThread(NULL, 0, TimedReadThread, Timed, 0, NULL);
        Sleep(500);
        ok(WriteCloseRecord(File, &TimedUsn), "close record for the timed read: %lu\n", GetLastError());
        ok(WaitForSingleObject(Thread, 15000) == WAIT_OBJECT_0, "timed read never returned\n");
        ok(Timed->Result && FindTestRecord(Timed->Buffer, Timed->Returned, TimedUsn) != NULL,
           "timed read: %d (%lu), %lu bytes\n", Timed->Result, Timed->Error, Timed->Returned);
        ok(Timed->Elapsed >= 1500, "timed read returned after %lu ms, before its timeout\n", Timed->Elapsed);
        CloseHandle(Thread);
        HeapFree(GetProcessHeap(), 0, Timed);
    }

TimedDone:
    CloseHandle(File);
    DeleteFileW(FilePath);
    CloseHandle(Volume);

    TestUnprivilegedOpen(Letter);
    TestWaitEdges();
}
