/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test named data streams on NTFS: Zone.Identifier on a file and a stream on a directory
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <strsafe.h>

#define ZONE_TEXT "[ZoneTransfer]\r\nZoneId=3\r\n"
#define HOST_TEXT "HostUrl=https://example.invalid/\r\n"
#define BASE_TEXT "base"
#define DIR_TEXT "directory stream data"

static WCHAR TestDir[MAX_PATH];

static BOOL
FindNtfsDir(VOID)
{
    WCHAR Root[] = L"?:\\";
    WCHAR FileSystem[MAX_PATH + 1];
    DWORD Flags = 0;

    if (!GetTempPathW(ARRAYSIZE(TestDir), TestDir))
        return FALSE;
    Root[0] = TestDir[0];
    if (!GetVolumeInformationW(Root, NULL, 0, NULL, NULL, &Flags, FileSystem, ARRAYSIZE(FileSystem)))
        return FALSE;
    if (wcscmp(FileSystem, L"NTFS"))
        return FALSE;
    ok(Flags & FILE_NAMED_STREAMS, "NTFS volume %S does not report FILE_NAMED_STREAMS (flags 0x%lx)\n", Root, Flags);
    return TRUE;
}

static PCWSTR
TestPath(PCWSTR Name, PWSTR Buffer)
{
    StringCchPrintfW(Buffer, MAX_PATH, L"%sntfsads_%s", TestDir, Name);
    return Buffer;
}

static BOOL
WriteData(PCWSTR Path, DWORD Disposition, DWORD Access, PCSTR Data)
{
    HANDLE File;
    DWORD Written = 0;
    DWORD Size = (DWORD)strlen(Data);
    BOOL Ok;

    File = CreateFileW(Path, Access, 0, NULL, Disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    if (Access == FILE_APPEND_DATA)
        SetFilePointer(File, 0, NULL, FILE_END);
    Ok = WriteFile(File, Data, Size, &Written, NULL);
    ok(Ok && Written == Size, "WriteFile(%S): %d, %lu of %lu written\n", Path, Ok, Written, Size);
    CloseHandle(File);
    return Ok && Written == Size;
}

static VOID
CheckData(PCWSTR Path, PCSTR Expected, PCSTR Scenario)
{
    CHAR Buffer[256];
    HANDLE File;
    DWORD Read = 0;
    DWORD Size = (DWORD)strlen(Expected);
    LARGE_INTEGER FileSize;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "%s: open %S failed: %lu\n", Scenario, Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(GetFileSizeEx(File, &FileSize) && FileSize.QuadPart == Size,
       "%s: size is %I64d, expected %lu\n", Scenario, FileSize.QuadPart, Size);
    ok(ReadFile(File, Buffer, sizeof(Buffer), &Read, NULL), "%s: ReadFile failed: %lu\n", Scenario, GetLastError());
    CloseHandle(File);
    ok(Read == Size && !memcmp(Buffer, Expected, Size), "%s: read %lu bytes, expected %lu of the written text\n",
       Scenario, Read, Size);
}

static VOID
CheckMissing(PCWSTR Path, PCSTR Scenario)
{
    HANDLE File;

    SetLastError(0xdeadbeef);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File == INVALID_HANDLE_VALUE, "%s: %S opened\n", Scenario, Path);
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "%s: error %lu, expected ERROR_FILE_NOT_FOUND\n",
       Scenario, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
}

static ULONG
CountStreams(PCWSTR Path, PCWSTR WantedName, PLONGLONG WantedSize)
{
    WIN32_FIND_STREAM_DATA Data;
    HANDLE Find;
    ULONG Count = 0;

    *WantedSize = -1;
    Find = FindFirstStreamW(Path, FindStreamInfoStandard, &Data, 0);
    ok(Find != INVALID_HANDLE_VALUE, "FindFirstStreamW(%S) failed: %lu\n", Path, GetLastError());
    if (Find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        Count++;
        if (!_wcsicmp(Data.cStreamName, WantedName))
            *WantedSize = Data.StreamSize.QuadPart;
    } while (FindNextStreamW(Find, &Data));
    FindClose(Find);
    return Count;
}

static VOID
TestFileZoneIdentifier(VOID)
{
    WCHAR Base[MAX_PATH], Zone[MAX_PATH], Missing[MAX_PATH], Fresh[MAX_PATH], FreshZone[MAX_PATH];
    LONGLONG Size;
    HANDLE File;

    TestPath(L"file.txt", Base);
    StringCchPrintfW(Zone, MAX_PATH, L"%s:Zone.Identifier", Base);
    StringCchPrintfW(Missing, MAX_PATH, L"%s:NoSuchStream", Base);
    DeleteFileW(Base);

    if (!WriteData(Base, CREATE_ALWAYS, GENERIC_WRITE, BASE_TEXT))
        return;
    CheckMissing(Zone, "before the stream exists");

    /* Mark-of-the-Web: create, read, append, read. */
    ok(WriteData(Zone, CREATE_ALWAYS, GENERIC_WRITE, ZONE_TEXT), "creating the Zone.Identifier stream failed\n");
    CheckData(Zone, ZONE_TEXT, "Zone.Identifier after create");
    ok(WriteData(Zone, OPEN_EXISTING, FILE_APPEND_DATA, HOST_TEXT), "appending to the stream failed\n");
    CheckData(Zone, ZONE_TEXT HOST_TEXT, "Zone.Identifier after append");
    CheckData(Base, BASE_TEXT, "unnamed stream after the stream writes");
    CheckMissing(Missing, "other stream name");

    /* CREATE_NEW on an existing stream collides; the name compares case-insensitively. */
    SetLastError(0xdeadbeef);
    File = CreateFileW(Zone, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS,
       "CREATE_NEW on an existing stream: handle %p, error %lu\n", File, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    StringCchPrintfW(Missing, MAX_PATH, L"%s:ZONE.IDENTIFIER", Base);
    CheckData(Missing, ZONE_TEXT HOST_TEXT, "stream opened with an upper-case name");

    /* Enumeration: the unnamed stream and the zone stream. */
    ok(CountStreams(Base, L":Zone.Identifier:$DATA", &Size) == 2, "expected 2 streams on the file\n");
    ok(Size == (LONGLONG)strlen(ZONE_TEXT HOST_TEXT), "Zone.Identifier enumerated with size %I64d\n", Size);

    /* Delete only the stream. */
    ok(DeleteFileW(Zone), "DeleteFileW(%S) failed: %lu\n", Zone, GetLastError());
    CheckMissing(Zone, "after deleting the stream");
    CheckData(Base, BASE_TEXT, "unnamed stream after deleting the named one");
    ok(CountStreams(Base, L":Zone.Identifier:$DATA", &Size) == 1 && Size == -1,
       "the deleted stream is still enumerated\n");

    /* The file and its stream created in one call. */
    TestPath(L"fresh.txt", Fresh);
    StringCchPrintfW(FreshZone, MAX_PATH, L"%s:Zone.Identifier", Fresh);
    DeleteFileW(Fresh);
    ok(WriteData(FreshZone, CREATE_NEW, GENERIC_WRITE, ZONE_TEXT), "creating file and stream together failed\n");
    CheckData(FreshZone, ZONE_TEXT, "stream of a file created through it");
    CheckData(Fresh, "", "unnamed stream of a file created through its named stream");

    DeleteFileW(Fresh);
    DeleteFileW(Base);
}

static VOID
TestDirectoryStream(VOID)
{
    WCHAR Dir[MAX_PATH], Stream[MAX_PATH];
    LONGLONG Size;

    TestPath(L"dir", Dir);
    StringCchPrintfW(Stream, MAX_PATH, L"%s:dirstream", Dir);
    RemoveDirectoryW(Dir);
    ok(CreateDirectoryW(Dir, NULL), "CreateDirectoryW(%S) failed: %lu\n", Dir, GetLastError());

    if (WriteData(Stream, CREATE_ALWAYS, GENERIC_WRITE, DIR_TEXT))
    {
        CheckData(Stream, DIR_TEXT, "named stream on a directory");
        ok(CountStreams(Dir, L":dirstream:$DATA", &Size) >= 1 && Size == (LONGLONG)strlen(DIR_TEXT),
           "the directory stream is enumerated with size %I64d\n", Size);
        ok(DeleteFileW(Stream), "DeleteFileW(%S) failed: %lu\n", Stream, GetLastError());
        CheckMissing(Stream, "after deleting the directory stream");
    }
    ok(RemoveDirectoryW(Dir), "RemoveDirectoryW(%S) failed: %lu\n", Dir, GetLastError());
}

START_TEST(NamedStreams)
{
    if (!FindNtfsDir())
    {
        skip("The temporary directory is not on NTFS\n");
        return;
    }
    TestFileZoneIdentifier();
    TestDirectoryStream();
}
