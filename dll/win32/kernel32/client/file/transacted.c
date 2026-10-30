/*
 * PROJECT:     LiberNT Win32 Base API
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Transacted file API entry points
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * LiberNT has no kernel transaction manager, so a transaction handle can never
 * be valid. A call without a transaction (NULL or INVALID_HANDLE_VALUE) is the
 * plain operation and is forwarded to the non-transacted API; a call that
 * carries a handle fails with ERROR_INVALID_TRANSACTION instead of pretending
 * to be atomic.
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS ********************************************************/

static
BOOL
BaseHasTransaction(
    _In_opt_ HANDLE hTransaction)
{
    if (hTransaction == NULL || hTransaction == INVALID_HANDLE_VALUE)
        return FALSE;

    SetLastError(ERROR_INVALID_TRANSACTION);
    return TRUE;
}

/* PUBLIC FUNCTIONS *********************************************************/

HANDLE
WINAPI
CreateFileTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile,
    _In_ HANDLE hTransaction,
    _In_opt_ PUSHORT pusMiniVersion,
    _Reserved_ PVOID lpExtendedParameter)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE
WINAPI
CreateFileTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile,
    _In_ HANDLE hTransaction,
    _In_opt_ PUSHORT pusMiniVersion,
    _Reserved_ PVOID lpExtendedParameter)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL
WINAPI
CopyFileTransactedW(
    _In_ LPCWSTR lpExistingFileName,
    _In_ LPCWSTR lpNewFileName,
    _In_opt_ LPPROGRESS_ROUTINE lpProgressRoutine,
    _In_opt_ LPVOID lpData,
    _In_opt_ LPBOOL pbCancel,
    _In_ DWORD dwCopyFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CopyFileExW(lpExistingFileName, lpNewFileName, lpProgressRoutine,
                       lpData, pbCancel, dwCopyFlags);
}

BOOL
WINAPI
CopyFileTransactedA(
    _In_ LPCSTR lpExistingFileName,
    _In_ LPCSTR lpNewFileName,
    _In_opt_ LPPROGRESS_ROUTINE lpProgressRoutine,
    _In_opt_ LPVOID lpData,
    _In_opt_ LPBOOL pbCancel,
    _In_ DWORD dwCopyFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CopyFileExA(lpExistingFileName, lpNewFileName, lpProgressRoutine,
                       lpData, pbCancel, dwCopyFlags);
}

BOOL
WINAPI
CreateDirectoryTransactedW(
    _In_opt_ LPCWSTR lpTemplateDirectory,
    _In_ LPCWSTR lpNewDirectory,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    if (lpTemplateDirectory)
        return CreateDirectoryExW(lpTemplateDirectory, lpNewDirectory, lpSecurityAttributes);

    return CreateDirectoryW(lpNewDirectory, lpSecurityAttributes);
}

BOOL
WINAPI
CreateDirectoryTransactedA(
    _In_opt_ LPCSTR lpTemplateDirectory,
    _In_ LPCSTR lpNewDirectory,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    if (lpTemplateDirectory)
        return CreateDirectoryExA(lpTemplateDirectory, lpNewDirectory, lpSecurityAttributes);

    return CreateDirectoryA(lpNewDirectory, lpSecurityAttributes);
}

BOOL
WINAPI
DeleteFileTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return DeleteFileW(lpFileName);
}

BOOL
WINAPI
DeleteFileTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return DeleteFileA(lpFileName);
}

BOOL
WINAPI
MoveFileTransactedW(
    _In_ LPCWSTR lpExistingFileName,
    _In_opt_ LPCWSTR lpNewFileName,
    _In_opt_ LPPROGRESS_ROUTINE lpProgressRoutine,
    _In_opt_ LPVOID lpData,
    _In_ DWORD dwFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return MoveFileWithProgressW(lpExistingFileName, lpNewFileName, lpProgressRoutine, lpData, dwFlags);
}

BOOL
WINAPI
MoveFileTransactedA(
    _In_ LPCSTR lpExistingFileName,
    _In_opt_ LPCSTR lpNewFileName,
    _In_opt_ LPPROGRESS_ROUTINE lpProgressRoutine,
    _In_opt_ LPVOID lpData,
    _In_ DWORD dwFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return MoveFileWithProgressA(lpExistingFileName, lpNewFileName, lpProgressRoutine, lpData, dwFlags);
}

BOOL
WINAPI
RemoveDirectoryTransactedW(
    _In_ LPCWSTR lpPathName,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return RemoveDirectoryW(lpPathName);
}

BOOL
WINAPI
RemoveDirectoryTransactedA(
    _In_ LPCSTR lpPathName,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return RemoveDirectoryA(lpPathName);
}
