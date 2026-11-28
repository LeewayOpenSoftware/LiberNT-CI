/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the SMB2 rename request validation and renaming on a live SMB2 share
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <stdint.h>
#include "smb2enc.h"
#include "smb2proto.h"

static uint32_t
Check(
    _In_ BYTE ReplaceIfExists,
    _In_ const WCHAR *Name,
    _In_ LONG NameBytesDelta,
    _In_ ULONGLONG RootDirectory,
    _In_ ULONG ExtraBytes)
{
    BYTE Buffer[SMB2ENC_RENAME_HEADER + 2 * 1100];
    ULONG NameBytes = (ULONG)wcslen(Name) * sizeof(WCHAR);
    ULONG Length = SMB2ENC_RENAME_HEADER + NameBytes + ExtraBytes;

    memset(Buffer, 0, sizeof(Buffer));
    Buffer[0] = ReplaceIfExists;
    memcpy(Buffer + 8, &RootDirectory, sizeof(RootDirectory));
    *(ULONG *)(Buffer + 16) = (ULONG)((LONG)NameBytes + NameBytesDelta);
    memcpy(Buffer + SMB2ENC_RENAME_HEADER, Name, NameBytes);
    return Smb2ValidateRenameInfo(Buffer, Length);
}

static void
TestValidation(void)
{
    BYTE Buffer[SMB2ENC_RENAME_HEADER + 4];
    WCHAR Long[SMB2ENC_MAX_NAME_CHARS + 2];
    ULONG Index;

    ok_hex(Check(0, L"a", 0, 0, 0), SMB2_STATUS_SUCCESS);
    ok_hex(Check(1, L"a", 0, 0, 0), SMB2_STATUS_SUCCESS);
    ok_hex(Check(0, L"dir\\new name.txt", 0, 0, 0), SMB2_STATUS_SUCCESS);

    ok_hex(Check(2, L"a", 0, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"a", 0, 1, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"a", 2, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"ab", -2, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"a", 0, 0, 2), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"a", 1, 0, 1), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"", 0, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"\\a", 0, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Check(0, L"a\\", 0, 0, 0), SMB2_STATUS_INVALID_PARAMETER);

    memset(Buffer, 0, sizeof(Buffer));
    *(ULONG *)(Buffer + 16) = 4;
    Buffer[SMB2ENC_RENAME_HEADER] = 'a';
    ok_hex(Smb2ValidateRenameInfo(Buffer, sizeof(Buffer)), SMB2_STATUS_INVALID_PARAMETER);

    ok_hex(Smb2ValidateRenameInfo(NULL, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateRenameInfo(Buffer, SMB2ENC_RENAME_HEADER), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateRenameInfo(Buffer, SMB2ENC_RENAME_HEADER - 1), SMB2_STATUS_INVALID_PARAMETER);

    for (Index = 0; Index < SMB2ENC_MAX_NAME_CHARS; Index++)
        Long[Index] = L'x';
    Long[SMB2ENC_MAX_NAME_CHARS] = 0;
    ok_hex(Check(0, Long, 0, 0, 0), SMB2_STATUS_SUCCESS);
    Long[SMB2ENC_MAX_NAME_CHARS] = L'x';
    Long[SMB2ENC_MAX_NAME_CHARS + 1] = 0;
    ok_hex(Check(0, Long, 0, 0, 0), SMB2_STATUS_INVALID_PARAMETER);
}

static BOOL
Exists(
    _In_ const WCHAR *Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL
Create(
    _In_ const WCHAR *Path,
    _In_ const char *Text)
{
    HANDLE File;
    DWORD Written;
    BOOL Ok;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Text, (DWORD)strlen(Text), &Written, NULL);
    CloseHandle(File);
    return Ok;
}

static BOOL
Contains(
    _In_ const WCHAR *Path,
    _In_ const char *Text)
{
    HANDLE File;
    char Data[64];
    DWORD Read = 0;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(File, Data, sizeof(Data), &Read, NULL);
    CloseHandle(File);
    return Read == strlen(Text) && memcmp(Data, Text, Read) == 0;
}

static void
TestLive(void)
{
    WCHAR Root[MAX_PATH], A[MAX_PATH], B[MAX_PATH], Dir[MAX_PATH], DirB[MAX_PATH], Moved[MAX_PATH];
    DWORD Length = GetEnvironmentVariableW(L"SMB2_TEST_ROOT", Root, RTL_NUMBER_OF(Root) - 64);

    if (!Length || Length >= RTL_NUMBER_OF(Root) - 64)
    {
        skip("SMB2_TEST_ROOT (a writable directory on an SMB2 share, for example \\\\server\\share\\tests) is not set\n");
        return;
    }

    _snwprintf(A, RTL_NUMBER_OF(A), L"%ls\\ren-a-%lu.txt", Root, GetCurrentProcessId());
    _snwprintf(B, RTL_NUMBER_OF(B), L"%ls\\ren-b-%lu.txt", Root, GetCurrentProcessId());
    _snwprintf(Dir, RTL_NUMBER_OF(Dir), L"%ls\\ren-d-%lu", Root, GetCurrentProcessId());
    _snwprintf(DirB, RTL_NUMBER_OF(DirB), L"%ls\\ren-e-%lu", Root, GetCurrentProcessId());
    _snwprintf(Moved, RTL_NUMBER_OF(Moved), L"%ls\\moved.txt", Dir);
    DeleteFileW(A);
    DeleteFileW(B);
    DeleteFileW(Moved);
    RemoveDirectoryW(Dir);
    RemoveDirectoryW(DirB);

    ok(Create(A, "alpha"), "Cannot create %ls, error %lu\n", A, GetLastError());

    ok(MoveFileExW(A, B, 0), "Rename of a file failed, error %lu\n", GetLastError());
    ok(!Exists(A), "The old name is still there\n");
    ok(Contains(B, "alpha"), "The renamed file lost its content\n");

    ok(Create(A, "beta"), "Cannot create the second file, error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!MoveFileExW(A, B, 0), "Rename over an existing file succeeded without replace\n");
    ok(GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS,
       "Wrong error %lu for a rename over an existing file\n", GetLastError());
    ok(Contains(B, "alpha"), "The target changed although the rename failed\n");

    ok(MoveFileExW(A, B, MOVEFILE_REPLACE_EXISTING), "Rename with replace failed, error %lu\n", GetLastError());
    ok(!Exists(A), "The old name is still there after the replace\n");
    ok(Contains(B, "beta"), "The replace did not bring the new content\n");

    ok(CreateDirectoryW(Dir, NULL), "Cannot create %ls, error %lu\n", Dir, GetLastError());
    ok(MoveFileExW(B, Moved, 0), "Rename into a subdirectory failed, error %lu\n", GetLastError());
    ok(!Exists(B) && Contains(Moved, "beta"), "The file did not arrive in the subdirectory\n");

    ok(MoveFileExW(Moved, B, 0), "Rename out of a subdirectory failed, error %lu\n", GetLastError());
    ok(MoveFileExW(Dir, DirB, 0), "Rename of a directory failed, error %lu\n", GetLastError());
    ok(!Exists(Dir) && Exists(DirB), "The directory was not renamed\n");

    SetLastError(0xdeadbeef);
    ok(!MoveFileExW(A, B, 0), "Rename of a missing file succeeded\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "Wrong error %lu for a missing source\n", GetLastError());

    DeleteFileW(B);
    RemoveDirectoryW(DirB);
}

START_TEST(Smb2Rename)
{
    TestValidation();
    TestLive();
}
