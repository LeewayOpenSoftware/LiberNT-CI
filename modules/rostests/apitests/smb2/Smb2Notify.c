/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the SMB2 change notify encoding and directory change watches on a live SMB2 share
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <stdint.h>
#include "smb2enc.h"
#include "smb2proto.h"

static void
PutEntry(BYTE *Buffer, ULONG Pos, ULONG Next, ULONG Action, ULONG NameBytes)
{
    Smb2Put32(Buffer, Pos, Next);
    Smb2Put32(Buffer, Pos + 4, Action);
    Smb2Put32(Buffer, Pos + 8, NameBytes);
}

static void
TestEncoding(void)
{
    BYTE Body[SMB2ENC_NOTIFY_BODY];
    BYTE Chain[64];

    memset(Body, 0xCC, sizeof(Body));
    Smb2BuildNotifyBody(Body, 0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL, 0x00000017, 1, 4096);
    ok(Body[0] == 32 && Body[1] == 0, "Wrong structure size %02x%02x\n", Body[1], Body[0]);
    ok(Body[2] == 1 && Body[3] == 0, "Wrong flags for a tree watch\n");
    ok(Body[4] == 0 && Body[5] == 0x10 && Body[6] == 0 && Body[7] == 0, "Wrong output length\n");
    ok(Smb2Get64(Body, 8) == 0x1122334455667788ULL, "Wrong persistent id\n");
    ok(Smb2Get64(Body, 16) == 0x99AABBCCDDEEFF00ULL, "Wrong volatile id\n");
    ok(Smb2Get32(Body, 24) == 0x17, "Wrong completion filter\n");
    ok(Smb2Get32(Body, 28) == 0, "The reserved field is not zero\n");
    Smb2BuildNotifyBody(Body, 1, 2, 3, 0, 100);
    ok(Body[2] == 0, "A single directory watch carries the tree flag\n");

    ok_hex(Smb2ValidateNotifyChain(NULL, 0), SMB2_STATUS_SUCCESS);

    memset(Chain, 0, sizeof(Chain));
    PutEntry(Chain, 0, 0, 1, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 16), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateNotifyChain(Chain, 15), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    ok_hex(Smb2ValidateNotifyChain(Chain, 11), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    PutEntry(Chain, 0, 16, 2, 4);
    PutEntry(Chain, 16, 0, 5, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateNotifyChain(Chain, 31), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    PutEntry(Chain, 0, 16, 0, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 16, 6, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);

    PutEntry(Chain, 0, 16, 1, 5);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 14, 1, 2);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 12, 1, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 40, 1, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 32), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 18, 1, 4);
    ok_hex(Smb2ValidateNotifyChain(Chain, 40), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 0, 1, 0xFFFFFFFE);
    ok_hex(Smb2ValidateNotifyChain(Chain, 16), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
    PutEntry(Chain, 0, 0, 1, 2 * SMB2ENC_MAX_NAME_CHARS + 2);
    ok_hex(Smb2ValidateNotifyChain(Chain, 12 + 2 * SMB2ENC_MAX_NAME_CHARS + 2), SMB2_STATUS_INVALID_NETWORK_RESPONSE);
}

static BOOL
Watch(
    _In_ HANDLE Dir,
    _In_ BOOL Tree,
    _Out_ BYTE *Buffer,
    _In_ DWORD Size,
    _In_ OVERLAPPED *Overlapped)
{
    memset(Overlapped, 0, sizeof(*Overlapped));
    Overlapped->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    return ReadDirectoryChangesW(Dir, Buffer, Size, Tree,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE,
                                 NULL, Overlapped, NULL);
}

static void
CheckFirst(
    _In_ const BYTE *Buffer,
    _In_ DWORD Action,
    _In_ const WCHAR *Name)
{
    const FILE_NOTIFY_INFORMATION *Info = (const FILE_NOTIFY_INFORMATION *)Buffer;

    ok(Info->Action == Action, "Wrong action %lu, expected %lu\n", Info->Action, Action);
    ok(Info->FileNameLength == wcslen(Name) * sizeof(WCHAR) && !memcmp(Info->FileName, Name, Info->FileNameLength),
       "Wrong name %.*ls, expected %ls\n", (int)(Info->FileNameLength / sizeof(WCHAR)), Info->FileName, Name);
}

static void
TestLive(void)
{
    WCHAR Root[MAX_PATH], Sub[MAX_PATH], Old[MAX_PATH], New[MAX_PATH];
    DWORD Length = GetEnvironmentVariableW(L"SMB2_TEST_ROOT", Root, RTL_NUMBER_OF(Root) - 96);
    HANDLE Dir, File;
    OVERLAPPED Overlapped, Overlapped2;
    BYTE Buffer[4096], Buffer2[4096];
    DWORD Transferred, Start, Error;
    WCHAR Name[64];

    if (!Length || Length >= RTL_NUMBER_OF(Root) - 96)
    {
        skip("SMB2_TEST_ROOT (a writable directory on an SMB2 share, for example \\\\server\\share\\tests) is not set\n");
        return;
    }

    _snwprintf(Sub, RTL_NUMBER_OF(Sub), L"%ls\\notify-%lu", Root, GetCurrentProcessId());
    ok(CreateDirectoryW(Sub, NULL), "Cannot create %ls, error %lu\n", Sub, GetLastError());
    Dir = CreateFileW(Sub, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    ok(Dir != INVALID_HANDLE_VALUE, "Cannot open the directory, error %lu\n", GetLastError());
    if (Dir == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(Sub);
        return;
    }

    _snwprintf(Old, RTL_NUMBER_OF(Old), L"%ls\\a.txt", Sub);
    _snwprintf(New, RTL_NUMBER_OF(New), L"%ls\\b.txt", Sub);

    ok(Watch(Dir, FALSE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
       "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    Sleep(300);
    ok(WaitForSingleObject(Overlapped.hEvent, 0) == WAIT_TIMEOUT, "The watch completed without a change\n");
    File = CreateFileW(Old, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Cannot create the file, error %lu\n", GetLastError());
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The watch did not report the creation\n");
    ok(GetOverlappedResult(Dir, &Overlapped, &Transferred, FALSE) && Transferred >= 12, "The watch returned %lu bytes\n", Transferred);
    CheckFirst(Buffer, FILE_ACTION_ADDED, L"a.txt");
    CloseHandle(Overlapped.hEvent);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);

    ok(Watch(Dir, FALSE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
       "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    ok(MoveFileW(Old, New), "Cannot rename, error %lu\n", GetLastError());
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The watch did not report the rename\n");
    ok(GetOverlappedResult(Dir, &Overlapped, &Transferred, FALSE) && Transferred >= 12, "The watch returned %lu bytes\n", Transferred);
    CheckFirst(Buffer, FILE_ACTION_RENAMED_OLD_NAME, L"a.txt");
    CloseHandle(Overlapped.hEvent);

    ok(Watch(Dir, FALSE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
       "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    ok(DeleteFileW(New), "Cannot delete, error %lu\n", GetLastError());
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The watch did not report the deletion\n");
    ok(GetOverlappedResult(Dir, &Overlapped, &Transferred, FALSE) && Transferred >= 12, "The watch returned %lu bytes\n", Transferred);
    CheckFirst(Buffer, FILE_ACTION_REMOVED, L"b.txt");
    CloseHandle(Overlapped.hEvent);

    ok(Watch(Dir, TRUE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
       "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    _snwprintf(Name, RTL_NUMBER_OF(Name), L"%ls\\d", Sub);
    ok(CreateDirectoryW(Name, NULL), "Cannot create the subdirectory, error %lu\n", GetLastError());
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The tree watch did not report the subdirectory\n");
    ok(GetOverlappedResult(Dir, &Overlapped, &Transferred, FALSE) && Transferred >= 12, "The watch returned %lu bytes\n", Transferred);
    CheckFirst(Buffer, FILE_ACTION_ADDED, L"d");
    CloseHandle(Overlapped.hEvent);
    RemoveDirectoryW(Name);

    for (Length = 0; Length < 4; Length++)
    {
        ok(Watch(Dir, FALSE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
           "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
        if (WaitForSingleObject(Overlapped.hEvent, 500) == WAIT_TIMEOUT)
            break;
        CloseHandle(Overlapped.hEvent);
    }
    Start = GetTickCount();
    ok(CancelIoEx(Dir, &Overlapped), "CancelIoEx failed, error %lu\n", GetLastError());
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The cancelled watch did not complete\n");
    SetLastError(0xdeadbeef);
    ok(!GetOverlappedResult(Dir, &Overlapped, &Transferred, FALSE), "The cancelled watch succeeded\n");
    Error = GetLastError();
    ok(Error == ERROR_OPERATION_ABORTED, "Wrong error %lu for a cancelled watch\n", Error);
    ok(GetTickCount() - Start < 3000, "The cancel took %lu ms\n", GetTickCount() - Start);
    CloseHandle(Overlapped.hEvent);

    ok(Watch(Dir, FALSE, Buffer, sizeof(Buffer), &Overlapped) || GetLastError() == ERROR_IO_PENDING,
       "ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    ok(Watch(Dir, FALSE, Buffer2, sizeof(Buffer2), &Overlapped2) || GetLastError() == ERROR_IO_PENDING,
       "The second ReadDirectoryChangesW failed, error %lu\n", GetLastError());
    Sleep(300);
    Start = GetTickCount();
    CloseHandle(Dir);
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "Closing the handle did not end the first watch\n");
    ok(WaitForSingleObject(Overlapped2.hEvent, 5000) == WAIT_OBJECT_0, "Closing the handle did not end the second watch\n");
    ok(GetTickCount() - Start < 3000, "Closing the handle took %lu ms\n", GetTickCount() - Start);
    CloseHandle(Overlapped.hEvent);
    CloseHandle(Overlapped2.hEvent);

    ok(RemoveDirectoryW(Sub), "Cannot remove %ls, error %lu\n", Sub, GetLastError());
}

START_TEST(Smb2Notify)
{
    TestEncoding();
    TestLive();
}
