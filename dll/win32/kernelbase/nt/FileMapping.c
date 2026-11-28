/*
 * PROJECT:         ReactOS Win32 Base API (Orosi KernelBase)
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         File mapping view functions
 *                  Moved from kernel32 client/file/filemap.c; kernelbase implements them on
 *                  Windows 11 26100 and kernel32 forwards.
 * COPYRIGHT:       Alex Ionescu (alex.ionescu@reactos.org)
 */

#include <stdio.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>

#define NTOS_MODE_USER
#include <ndk/cmfuncs.h>
#include <ndk/exfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>

#include <ntstrsafe.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

/* kernel32_shared */
DWORD
WINAPI
BaseSetLastNTError(IN NTSTATUS Status);

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
LPVOID
NTAPI
MapViewOfFileEx(HANDLE hFileMappingObject,
                DWORD dwDesiredAccess,
                DWORD dwFileOffsetHigh,
                DWORD dwFileOffsetLow,
                SIZE_T dwNumberOfBytesToMap,
                LPVOID lpBaseAddress)
{
    NTSTATUS Status;
    LARGE_INTEGER SectionOffset;
    SIZE_T ViewSize;
    ULONG Protect;
    LPVOID ViewBase;

    /* Convert the offset */
    SectionOffset.LowPart = dwFileOffsetLow;
    SectionOffset.HighPart = dwFileOffsetHigh;

    /* Save the size and base */
    ViewBase = lpBaseAddress;
    ViewSize = dwNumberOfBytesToMap;

    /* Convert flags to NT Protection Attributes */
    if ((dwDesiredAccess & ~FILE_MAP_EXECUTE) == FILE_MAP_COPY)
    {
        Protect = (dwDesiredAccess & FILE_MAP_EXECUTE) ?
                   PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
    }
    else if (dwDesiredAccess & FILE_MAP_WRITE)
    {
        Protect = (dwDesiredAccess & FILE_MAP_EXECUTE) ?
                   PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    }
    else if (dwDesiredAccess & FILE_MAP_READ)
    {
        Protect = (dwDesiredAccess & FILE_MAP_EXECUTE) ?
                   PAGE_EXECUTE_READ : PAGE_READONLY;
    }
    else
    {
        Protect = PAGE_NOACCESS;
    }

    /* Map the section */
    Status = NtMapViewOfSection(hFileMappingObject,
                                NtCurrentProcess(),
                                &ViewBase,
                                0,
                                0,
                                &SectionOffset,
                                &ViewSize,
                                ViewShare,
                                0,
                                Protect);
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return NULL;
    }

    /* Return the base */
    return ViewBase;
}

/*
 * @implemented
 */
LPVOID
NTAPI
MapViewOfFile(HANDLE hFileMappingObject,
              DWORD dwDesiredAccess,
              DWORD dwFileOffsetHigh,
              DWORD dwFileOffsetLow,
              SIZE_T dwNumberOfBytesToMap)
{
    /* Call the extended API */
    return MapViewOfFileEx(hFileMappingObject,
                           dwDesiredAccess,
                           dwFileOffsetHigh,
                           dwFileOffsetLow,
                           dwNumberOfBytesToMap,
                           NULL);
}

/*
 * @implemented
 */
BOOL
NTAPI
UnmapViewOfFile(LPCVOID lpBaseAddress)
{
    NTSTATUS Status;

    /* Unmap the section */
    Status = NtUnmapViewOfSection(NtCurrentProcess(), (PVOID)lpBaseAddress);
    if (!NT_SUCCESS(Status))
    {
        /* Check if the pages were protected */
        if (Status == STATUS_INVALID_PAGE_PROTECTION || Status == STATUS_UNABLE_TO_FREE_VM)
        {
            /* Flush the region if it was a "secure memory cache" */
            if (RtlFlushSecureMemoryCache((PVOID)lpBaseAddress, 0))
            {
                /* Now try to unmap again */
                Status = NtUnmapViewOfSection(NtCurrentProcess(), (PVOID)lpBaseAddress);
                if (NT_SUCCESS(Status)) return TRUE;
            }
        }

        /* We failed */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Otherwise, return success */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
NTAPI
FlushViewOfFile(IN LPCVOID lpBaseAddress,
                IN SIZE_T dwNumberOfBytesToFlush)
{
    NTSTATUS Status;
    PVOID BaseAddress = (PVOID)lpBaseAddress;
    SIZE_T NumberOfBytesToFlush = dwNumberOfBytesToFlush;
    IO_STATUS_BLOCK IoStatusBlock;

    /* Flush the view */
    Status = NtFlushVirtualMemory(NtCurrentProcess(),
                                  &BaseAddress,
                                  &NumberOfBytesToFlush,
                                  &IoStatusBlock);
    if (!NT_SUCCESS(Status) && (Status != STATUS_NOT_MAPPED_DATA))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}
/* EOF */
