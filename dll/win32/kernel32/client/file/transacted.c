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

WINBASEAPI HANDLE WINAPI FindFirstFileNameW(_In_ LPCWSTR, _In_ DWORD, _Inout_ LPDWORD, _Out_ PWSTR);

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

/* Remaining transacted entry points (same rule as above) *******************/

BOOL
WINAPI
CreateHardLinkTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ LPCWSTR lpExistingFileName,
    _Reserved_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CreateHardLinkW(lpFileName, lpExistingFileName, lpSecurityAttributes);
}

BOOL
WINAPI
CreateHardLinkTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ LPCSTR lpExistingFileName,
    _Reserved_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CreateHardLinkA(lpFileName, lpExistingFileName, lpSecurityAttributes);
}

BOOLEAN
WINAPI
CreateSymbolicLinkTransactedW(
    _In_ LPCWSTR lpSymlinkFileName,
    _In_ LPCWSTR lpTargetFileName,
    _In_ DWORD dwFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CreateSymbolicLinkW(lpSymlinkFileName, lpTargetFileName, dwFlags);
}

BOOLEAN
WINAPI
CreateSymbolicLinkTransactedA(
    _In_ LPCSTR lpSymlinkFileName,
    _In_ LPCSTR lpTargetFileName,
    _In_ DWORD dwFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return CreateSymbolicLinkA(lpSymlinkFileName, lpTargetFileName, dwFlags);
}

HANDLE
WINAPI
FindFirstFileNameTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwFlags,
    _Inout_ LPDWORD StringLength,
    _Out_ PWSTR LinkName,
    _In_opt_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return FindFirstFileNameW(lpFileName, dwFlags, StringLength, LinkName);
}

HANDLE
WINAPI
FindFirstFileTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ FINDEX_INFO_LEVELS fInfoLevelId,
    _Out_ LPVOID lpFindFileData,
    _In_ FINDEX_SEARCH_OPS fSearchOp,
    _Reserved_ LPVOID lpSearchFilter,
    _In_ DWORD dwAdditionalFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return FindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData,
                            fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

HANDLE
WINAPI
FindFirstFileTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ FINDEX_INFO_LEVELS fInfoLevelId,
    _Out_ LPVOID lpFindFileData,
    _In_ FINDEX_SEARCH_OPS fSearchOp,
    _Reserved_ LPVOID lpSearchFilter,
    _In_ DWORD dwAdditionalFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return FindFirstFileExA(lpFileName, fInfoLevelId, lpFindFileData,
                            fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

HANDLE
WINAPI
FindFirstStreamTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ STREAM_INFO_LEVELS InfoLevel,
    _Out_ LPVOID lpFindStreamData,
    _Reserved_ DWORD dwFlags,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_HANDLE_VALUE;

    return FindFirstStreamW(lpFileName, InfoLevel, lpFindStreamData, dwFlags);
}

DWORD
WINAPI
GetCompressedFileSizeTransactedW(
    _In_ LPCWSTR lpFileName,
    _Out_opt_ LPDWORD lpFileSizeHigh,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_FILE_SIZE;

    return GetCompressedFileSizeW(lpFileName, lpFileSizeHigh);
}

DWORD
WINAPI
GetCompressedFileSizeTransactedA(
    _In_ LPCSTR lpFileName,
    _Out_opt_ LPDWORD lpFileSizeHigh,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return INVALID_FILE_SIZE;

    return GetCompressedFileSizeA(lpFileName, lpFileSizeHigh);
}

BOOL
WINAPI
GetFileAttributesTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_ LPVOID lpFileInformation,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

BOOL
WINAPI
GetFileAttributesTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_ LPVOID lpFileInformation,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

DWORD
WINAPI
GetFullPathNameTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD nBufferLength,
    _Out_ LPWSTR lpBuffer,
    _Out_opt_ LPWSTR *lpFilePart,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return 0;

    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

DWORD
WINAPI
GetFullPathNameTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ DWORD nBufferLength,
    _Out_ LPSTR lpBuffer,
    _Out_opt_ LPSTR *lpFilePart,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return 0;

    return GetFullPathNameA(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

DWORD
WINAPI
GetLongPathNameTransactedW(
    _In_ LPCWSTR lpszShortPath,
    _Out_ LPWSTR lpszLongPath,
    _In_ DWORD cchBuffer,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return 0;

    return GetLongPathNameW(lpszShortPath, lpszLongPath, cchBuffer);
}

DWORD
WINAPI
GetLongPathNameTransactedA(
    _In_ LPCSTR lpszShortPath,
    _Out_ LPSTR lpszLongPath,
    _In_ DWORD cchBuffer,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return 0;

    return GetLongPathNameA(lpszShortPath, lpszLongPath, cchBuffer);
}

BOOL
WINAPI
SetFileAttributesTransactedW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwFileAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return SetFileAttributesW(lpFileName, dwFileAttributes);
}

BOOL
WINAPI
SetFileAttributesTransactedA(
    _In_ LPCSTR lpFileName,
    _In_ DWORD dwFileAttributes,
    _In_ HANDLE hTransaction)
{
    if (BaseHasTransaction(hTransaction))
        return FALSE;

    return SetFileAttributesA(lpFileName, dwFileAttributes);
}
