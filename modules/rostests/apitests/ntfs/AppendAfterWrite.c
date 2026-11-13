/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that a file just written and closed reports its new size to the next open
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <strsafe.h>

#define HEAD_SIZE 193
#define MARK "MARK-APPENDED-AFTER-WRITE\r\n"
#define MARK_SIZE (sizeof(MARK) - 1)

static WCHAR TestDir[MAX_PATH];
static UCHAR Head[HEAD_SIZE];

static BOOL
FindNtfsDir(VOID)
{
    WCHAR Root[] = L"?:\\";
    WCHAR FileSystem[MAX_PATH + 1];

    if (!GetTempPathW(ARRAYSIZE(TestDir), TestDir))
        return FALSE;
    Root[0] = TestDir[0];
    if (!GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, FileSystem, ARRAYSIZE(FileSystem)))
        return FALSE;
    return !wcscmp(FileSystem, L"NTFS");
}

static PCWSTR
TestPath(PCWSTR Name)
{
    static WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s%s", TestDir, Name);
    return Path;
}

static BOOL
WriteHead(PCWSTR Path, DWORD Size)
{
    HANDLE File;
    DWORD Written = 0;
    BOOL Ok;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Head, Size, &Written, NULL);
    ok(Ok && Written == Size, "WriteFile head: %d, %lu written\n", Ok, Written);
    CloseHandle(File);
    return Ok && Written == Size;
}

static VOID
CheckContents(PCWSTR Path, DWORD HeadSize, PCSTR Scenario)
{
    UCHAR Buffer[HEAD_SIZE + 2 * MARK_SIZE];
    HANDLE File;
    DWORD Read = 0;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "%s: reopen for read failed: %lu\n", Scenario, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(ReadFile(File, Buffer, sizeof(Buffer), &Read, NULL), "%s: ReadFile failed: %lu\n", Scenario, GetLastError());
    CloseHandle(File);

    ok(Read == HeadSize + MARK_SIZE, "%s: file is %lu bytes, expected %lu\n",
       Scenario, Read, (ULONG)(HeadSize + MARK_SIZE));
    if (Read >= HeadSize)
        ok(!memcmp(Buffer, Head, HeadSize), "%s: the head of the file was overwritten\n", Scenario);
    if (Read == HeadSize + MARK_SIZE)
        ok(!memcmp(Buffer + HeadSize, MARK, MARK_SIZE), "%s: the mark is not at the end\n", Scenario);
}

static VOID
TestSeekToEnd(DWORD HeadSize)
{
    PCWSTR Path = TestPath(L"AppendAfterWrite-seek.txt");
    LARGE_INTEGER Size;
    HANDLE File;
    DWORD Written = 0, Position;

    if (!WriteHead(Path, HeadSize))
        return;

    File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "reopen for append failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        goto Cleanup;
    ok(GetFileSizeEx(File, &Size), "GetFileSizeEx failed: %lu\n", GetLastError());
    ok(Size.QuadPart == HeadSize, "GetFileSizeEx after reopen: %I64d, expected %lu\n", Size.QuadPart, HeadSize);
    Position = SetFilePointer(File, 0, NULL, FILE_END);
    ok(Position == HeadSize, "SetFilePointer(FILE_END): %lu, expected %lu\n", Position, HeadSize);
    ok(WriteFile(File, MARK, MARK_SIZE, &Written, NULL), "WriteFile mark failed: %lu\n", GetLastError());
    CloseHandle(File);
    CheckContents(Path, HeadSize, "seek to end");

Cleanup:
    DeleteFileW(Path);
}

static VOID
TestAppendAccess(DWORD HeadSize)
{
    PCWSTR Path = TestPath(L"AppendAfterWrite-access.txt");
    HANDLE File;
    DWORD Written = 0;

    if (!WriteHead(Path, HeadSize))
        return;

    File = CreateFileW(Path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "reopen with FILE_APPEND_DATA failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        goto Cleanup;
    ok(WriteFile(File, MARK, MARK_SIZE, &Written, NULL), "WriteFile mark failed: %lu\n", GetLastError());
    CloseHandle(File);
    CheckContents(Path, HeadSize, "append access");

Cleanup:
    DeleteFileW(Path);
}

static VOID
TestSecondHandle(DWORD HeadSize)
{
    PCWSTR Path = TestPath(L"AppendAfterWrite-open.txt");
    LARGE_INTEGER Size;
    HANDLE File, Second;
    DWORD Written = 0;

    File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteFile(File, Head, HeadSize, &Written, NULL), "WriteFile head failed: %lu\n", GetLastError());
    ok(GetFileSizeEx(File, &Size), "GetFileSizeEx failed: %lu\n", GetLastError());
    ok(Size.QuadPart == HeadSize, "GetFileSizeEx on the writing handle: %I64d, expected %lu\n",
       Size.QuadPart, HeadSize);

    Second = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    ok(Second != INVALID_HANDLE_VALUE, "second open failed: %lu\n", GetLastError());
    if (Second != INVALID_HANDLE_VALUE)
    {
        ok(GetFileSizeEx(Second, &Size), "GetFileSizeEx failed: %lu\n", GetLastError());
        ok(Size.QuadPart == HeadSize, "GetFileSizeEx on a second handle: %I64d, expected %lu\n",
           Size.QuadPart, HeadSize);
        CloseHandle(Second);
    }
    CloseHandle(File);
    DeleteFileW(Path);
}

START_TEST(AppendAfterWrite)
{
    ULONG i;

    if (!FindNtfsDir())
    {
        skip("The temporary directory is not on an NTFS volume\n");
        return;
    }
    for (i = 0; i < HEAD_SIZE; i++)
        Head[i] = (UCHAR)('a' + i % 26);

    TestSeekToEnd(HEAD_SIZE);
    TestSeekToEnd(13);
    TestAppendAccess(HEAD_SIZE);
    TestSecondHandle(HEAD_SIZE);
}
