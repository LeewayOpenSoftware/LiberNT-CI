/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Post-install self-test for the bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

static DWORD
BsSelfTestFiles(
    _In_ const BS_TARGET *Target,
    _In_ const BS_INVENTORY *Inventory)
{
    ULONG i;

    for (i = 0; i < Inventory->FileCount; i++)
    {
        char Hex[65];

        if (BsHashTargetFile(Target, Inventory->Files[i].Path, Hex) != ERROR_SUCCESS)
            return ERROR_FILE_NOT_FOUND;
        if (_stricmp(Hex, Inventory->Files[i].Sha256) != 0)
            return ERROR_CRC; /* a file on disk differs from the bundle */
    }
    return ERROR_SUCCESS;
}

static DWORD
BsSelfTestPinnedKey(
    _In_ const BS_TARGET *Target)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE Handle;
    uint8_t Buffer[S1_RSA2048_SIZE];
    DWORD Read;

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_DEVKEY_FILE) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!ReadFile(Handle, Buffer, sizeof(Buffer), &Read, NULL) || Read != sizeof(Buffer))
    {
        CloseHandle(Handle);
        return ERROR_READ_FAULT;
    }
    CloseHandle(Handle);
    return memcmp(Buffer, BsDevPublicKeyBytes(), S1_RSA2048_SIZE) == 0
               ? ERROR_SUCCESS
               : ERROR_INVALID_DATA;
}

/* Exercises the backup/rename/restore primitives on a scratch file so a
 * failed self-test never touches the real store. */
static DWORD
BsSelfTestSimulatedTransaction(
    _In_ const BS_TARGET *Target)
{
    static const WCHAR *Scratch = L"LiberNT\\Servicing\\bootstrap\\selftest.tmp";
    static const WCHAR *Backup = L"LiberNT\\Servicing\\bootstrap\\selftest.tmp.bak";
    WCHAR Original[] = L"bootstrap-selftest";
    WCHAR ScratchPath[BS_MAX_PATH_BUF], BackupPath[BS_MAX_PATH_BUF];
    HANDLE Handle;
    DWORD Written;
    DWORD Error;

    Error = BsBuildTargetPath(ScratchPath, RTL_NUMBER_OF(ScratchPath), Target, Scratch);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(BackupPath, RTL_NUMBER_OF(BackupPath), Target, Backup);
    if (Error != ERROR_SUCCESS)
        return Error;

    Handle = CreateFileW(ScratchPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WriteFile(Handle, Original, sizeof(Original), &Written, NULL) ||
        Written != sizeof(Original))
    {
        CloseHandle(Handle);
        return GetLastError();
    }
    CloseHandle(Handle);

    if (!MoveFileW(ScratchPath, BackupPath))
        return GetLastError();
    if (!MoveFileW(BackupPath, ScratchPath))
        return GetLastError();
    if (!DeleteFileW(ScratchPath))
        return GetLastError();
    return ERROR_SUCCESS;
}

DWORD
BsSelfTest(
    _In_ const BS_TARGET *Target,
    _In_ const BS_BUNDLE *Bundle,
    _In_ const BS_INVENTORY *Inventory,
    _In_ BOOL OnlineService)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    BOOL Exists;
    DWORD Error;

    (void)Bundle;

    Error = BsSelfTestFiles(Target, Inventory);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_SERVICING_DIR);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsCheckSecure(Path);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsSelfTestPinnedKey(Target);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsSelfTestSimulatedTransaction(Target);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (OnlineService)
    {
        Error = BsServiceQuery(&Exists);
        if (Error != ERROR_SUCCESS)
            return Error;
        if (!Exists)
            return ERROR_SERVICE_DOES_NOT_EXIST;
    }
    return ERROR_SUCCESS;
}
