/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the SMB2 byte-range lock request encoding and locking on a live SMB2 share
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <stdint.h>
#include "smb2enc.h"
#include "smb2proto.h"

static void
TestEncoding(void)
{
    BYTE Body[SMB2ENC_LOCK_BODY];
    BYTE Expected[SMB2ENC_LOCK_BODY];

    ok_hex(Smb2ValidateLockRange(0, 10, SMB2_LOCKFLAG_SHARED), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateLockRange(5, 0, SMB2_LOCKFLAG_EXCLUSIVE), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateLockRange(0x100, 0x200, SMB2_LOCKFLAG_UNLOCK), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateLockRange(0, UINT64_MAX, SMB2_LOCKFLAG_EXCLUSIVE), SMB2_STATUS_SUCCESS);
    ok_hex(Smb2ValidateLockRange(UINT64_MAX, 0, SMB2_LOCKFLAG_EXCLUSIVE), SMB2_STATUS_SUCCESS);

    ok_hex(Smb2ValidateLockRange(1, UINT64_MAX, SMB2_LOCKFLAG_EXCLUSIVE), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(UINT64_MAX, 1, SMB2_LOCKFLAG_SHARED), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(0, 1, 0), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(0, 1, SMB2_LOCKFLAG_SHARED | SMB2_LOCKFLAG_EXCLUSIVE), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(0, 1, SMB2_LOCKFLAG_SHARED | SMB2_LOCKFLAG_UNLOCK), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(0, 1, SMB2_LOCKFLAG_FAIL_IMMEDIATELY), SMB2_STATUS_INVALID_PARAMETER);
    ok_hex(Smb2ValidateLockRange(0, 1, 0x80), SMB2_STATUS_INVALID_PARAMETER);

    memset(Body, 0xCC, sizeof(Body));
    Smb2BuildLockBody(Body, 0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL, 0x0102030405060708ULL, 0x1011121314151617ULL,
                      SMB2_LOCKFLAG_EXCLUSIVE);
    memset(Expected, 0, sizeof(Expected));
    Expected[0] = 48;
    Expected[2] = 1;
    Expected[8] = 0x88; Expected[9] = 0x77; Expected[10] = 0x66; Expected[11] = 0x55;
    Expected[12] = 0x44; Expected[13] = 0x33; Expected[14] = 0x22; Expected[15] = 0x11;
    Expected[16] = 0x00; Expected[17] = 0xFF; Expected[18] = 0xEE; Expected[19] = 0xDD;
    Expected[20] = 0xCC; Expected[21] = 0xBB; Expected[22] = 0xAA; Expected[23] = 0x99;
    Expected[24] = 0x08; Expected[25] = 0x07; Expected[26] = 0x06; Expected[27] = 0x05;
    Expected[28] = 0x04; Expected[29] = 0x03; Expected[30] = 0x02; Expected[31] = 0x01;
    Expected[32] = 0x17; Expected[33] = 0x16; Expected[34] = 0x15; Expected[35] = 0x14;
    Expected[36] = 0x13; Expected[37] = 0x12; Expected[38] = 0x11; Expected[39] = 0x10;
    Expected[40] = 0x12;
    ok(memcmp(Body, Expected, sizeof(Body)) == 0, "The lock request body does not match the wire layout\n");

    Smb2BuildLockBody(Body, 1, 2, 3, 4, SMB2_LOCKFLAG_UNLOCK);
    ok(Body[40] == 0x14 && Body[41] == 0 && Body[42] == 0 && Body[43] == 0, "Unlock flags are %02x%02x%02x%02x\n",
       Body[43], Body[42], Body[41], Body[40]);
    ok(Body[44] == 0 && Body[45] == 0 && Body[46] == 0 && Body[47] == 0, "The reserved field is not zero\n");
}

typedef struct
{
    HANDLE File;
    DWORD Delay;
} RELEASE;

static DWORD WINAPI
ReleaseLater(
    _In_ LPVOID Parameter)
{
    RELEASE *Release = Parameter;
    OVERLAPPED Overlapped;

    Sleep(Release->Delay);
    memset(&Overlapped, 0, sizeof(Overlapped));
    return UnlockFileEx(Release->File, 0, 100, 0, &Overlapped) ? 0 : GetLastError();
}

static BOOL
Lock(
    _In_ HANDLE File,
    _In_ DWORD Offset,
    _In_ DWORD Length,
    _In_ DWORD Flags)
{
    OVERLAPPED Overlapped;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.Offset = Offset;
    return LockFileEx(File, Flags, 0, Length, 0, &Overlapped);
}

static BOOL
Unlock(
    _In_ HANDLE File,
    _In_ DWORD Offset,
    _In_ DWORD Length)
{
    OVERLAPPED Overlapped;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.Offset = Offset;
    return UnlockFileEx(File, 0, Length, 0, &Overlapped);
}

static BOOL
CanRead(
    _In_ HANDLE File,
    _In_ DWORD Offset)
{
    char Data[4];
    DWORD Read;
    OVERLAPPED Overlapped;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.Offset = Offset;
    return ReadFile(File, Data, sizeof(Data), &Read, &Overlapped);
}

#define LOCK_X  (LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)
#define LOCK_S  LOCKFILE_FAIL_IMMEDIATELY

static void
TestLive(void)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH];
    DWORD Length = GetEnvironmentVariableW(L"SMB2_TEST_ROOT", Root, RTL_NUMBER_OF(Root) - 64);
    HANDLE A, B, Thread;
    RELEASE Release;
    char Fill[512];
    DWORD Written, ThreadStatus, Start;

    if (!Length || Length >= RTL_NUMBER_OF(Root) - 64)
    {
        skip("SMB2_TEST_ROOT (a writable directory on an SMB2 share, for example \\\\server\\share\\tests) is not set\n");
        return;
    }

    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%ls\\lock-%lu.bin", Root, GetCurrentProcessId());
    memset(Fill, 'x', sizeof(Fill));
    A = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(A != INVALID_HANDLE_VALUE, "Cannot create %ls, error %lu\n", Path, GetLastError());
    if (A == INVALID_HANDLE_VALUE)
        return;
    ok(WriteFile(A, Fill, sizeof(Fill), &Written, NULL) && Written == sizeof(Fill), "Cannot fill the file\n");
    B = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(B != INVALID_HANDLE_VALUE, "Cannot open the second handle, error %lu\n", GetLastError());
    if (B == INVALID_HANDLE_VALUE)
    {
        CloseHandle(A);
        return;
    }

    ok(Lock(A, 0, 100, LOCK_X), "Exclusive lock failed, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!Lock(B, 50, 10, LOCK_X), "An exclusive lock over a locked range succeeded\n");
    ok(GetLastError() == ERROR_LOCK_VIOLATION, "Wrong error %lu for a lock conflict\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!Lock(B, 0, 1, LOCK_S), "A shared lock over an exclusive range succeeded\n");
    ok(GetLastError() == ERROR_LOCK_VIOLATION, "Wrong error %lu for a shared conflict\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!CanRead(B, 10), "Reading a range locked by another handle succeeded\n");
    ok(GetLastError() == ERROR_LOCK_VIOLATION, "Wrong error %lu for a read conflict\n", GetLastError());
    ok(CanRead(A, 10), "The owner cannot read its own range\n");
    ok(CanRead(B, 200), "A read outside the locked range failed\n");
    ok(Lock(B, 100, 10, LOCK_X), "A lock next to the locked range failed, error %lu\n", GetLastError());
    ok(Unlock(B, 100, 10), "Unlock of the neighbour failed, error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!Unlock(A, 0, 50), "Unlock of a part of a lock succeeded\n");
    ok(GetLastError() == ERROR_NOT_LOCKED, "Wrong error %lu for a partial unlock\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!Unlock(B, 0, 100), "Unlock by a handle that does not hold the lock succeeded\n");
    ok(GetLastError() == ERROR_NOT_LOCKED, "Wrong error %lu for a foreign unlock\n", GetLastError());

    ok(Unlock(A, 0, 100), "Unlock failed, error %lu\n", GetLastError());
    ok(CanRead(B, 10), "Reading after the unlock failed\n");

    ok(Lock(A, 0, 100, LOCK_S), "Shared lock failed, error %lu\n", GetLastError());
    ok(Lock(B, 0, 100, LOCK_S), "A second shared lock failed, error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!Lock(B, 0, 100, LOCK_X), "An exclusive lock over shared locks succeeded\n");
    ok(Unlock(A, 0, 100) && Unlock(B, 0, 100), "Unlock of the shared locks failed\n");

    ok(Lock(A, 0, 100, LOCK_X), "Exclusive lock failed again, error %lu\n", GetLastError());
    Release.File = A;
    Release.Delay = 1000;
    Thread = CreateThread(NULL, 0, ReleaseLater, &Release, 0, NULL);
    ok(Thread != NULL, "Cannot start the helper thread\n");
    Start = GetTickCount();
    ok(Lock(B, 0, 100, LOCKFILE_EXCLUSIVE_LOCK), "A waiting lock did not get the range, error %lu\n", GetLastError());
    ok(GetTickCount() - Start >= 500, "The waiting lock returned before the range was free\n");
    if (Thread)
    {
        WaitForSingleObject(Thread, INFINITE);
        ok(GetExitCodeThread(Thread, &ThreadStatus) && ThreadStatus == 0, "The helper could not unlock, error %lu\n",
           ThreadStatus);
        CloseHandle(Thread);
    }

    CloseHandle(B);
    ok(Lock(A, 0, 100, LOCK_X), "Closing the holder did not free the range, error %lu\n", GetLastError());
    CloseHandle(A);
    DeleteFileW(Path);
}

START_TEST(Smb2Lock)
{
    TestEncoding();
    TestLive();
}
