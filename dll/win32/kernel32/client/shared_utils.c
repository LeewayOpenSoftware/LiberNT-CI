/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/utils_shared.c
 * PURPOSE:         Utility functions shared with kernel32_vista
 * PROGRAMMER:      Thomas Faber
*/

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

/*
* @implemented
*/
DWORD
WINAPI
BaseSetLastNTError(IN NTSTATUS Status)
{
    DWORD dwErrCode;

    /* Convert from NT to Win32, then set */
    dwErrCode = RtlNtStatusToDosError(Status);
    SetLastError(dwErrCode);
    return dwErrCode;
}

static
NTSTATUS
NTAPI
BasepQueryEnvironmentMaximumSize(
    _In_ const VOID *Environment,
    _Out_ PSIZE_T MaximumSize)
{
    MEMORY_BASIC_INFORMATION MemoryInfo;
    ULONG_PTR BaseAddress, EnvironmentAddress;
    NTSTATUS Status;
    SIZE_T Offset;

    if (!Environment || !MaximumSize)
        return STATUS_INVALID_PARAMETER;

    Status = NtQueryVirtualMemory(NtCurrentProcess(),
                                  (PVOID)Environment,
                                  MemoryBasicInformation,
                                  &MemoryInfo,
                                  sizeof(MemoryInfo),
                                  NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    BaseAddress = (ULONG_PTR)MemoryInfo.BaseAddress;
    EnvironmentAddress = (ULONG_PTR)Environment;
    if (EnvironmentAddress < BaseAddress)
        return STATUS_INVALID_PARAMETER;

    Offset = EnvironmentAddress - BaseAddress;
    if (Offset >= MemoryInfo.RegionSize)
        return STATUS_INVALID_PARAMETER;

    *MaximumSize = MemoryInfo.RegionSize - Offset;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
BasepQueryEnvironmentSizeA(
    _In_ PCSTR Environment,
    _Out_ PSIZE_T EnvironmentSize)
{
    SIZE_T MaximumSize, Index;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    Status = BasepQueryEnvironmentMaximumSize(Environment, &MaximumSize);
    if (!NT_SUCCESS(Status))
        return Status;

    if (MaximumSize < 2 * sizeof(CHAR))
        return STATUS_INVALID_PARAMETER;

    __TRY
    {
        for (Index = 0; Index + 1 < MaximumSize; ++Index)
        {
            if (Environment[Index] == ANSI_NULL &&
                Environment[Index + 1] == ANSI_NULL)
            {
                *EnvironmentSize = (Index + 2) * sizeof(CHAR);
                Found = TRUE;
                break;
            }
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        Status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY;

    if (Found)
        return STATUS_SUCCESS;

    return NT_SUCCESS(Status) ? STATUS_INVALID_PARAMETER : Status;
}

NTSTATUS
NTAPI
BasepQueryEnvironmentSizeW(
    _In_ PCWSTR Environment,
    _Out_ PSIZE_T EnvironmentSize)
{
    SIZE_T MaximumSize, MaximumLength, Index;
    NTSTATUS Status;
    BOOLEAN Found = FALSE;

    Status = BasepQueryEnvironmentMaximumSize(Environment, &MaximumSize);
    if (!NT_SUCCESS(Status))
        return Status;

    MaximumLength = MaximumSize / sizeof(WCHAR);
    if (MaximumLength < 2)
        return STATUS_INVALID_PARAMETER;

    __TRY
    {
        for (Index = 0; Index + 1 < MaximumLength; ++Index)
        {
            if (Environment[Index] == UNICODE_NULL &&
                Environment[Index + 1] == UNICODE_NULL)
            {
                *EnvironmentSize = (Index + 2) * sizeof(WCHAR);
                Found = TRUE;
                break;
            }
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        Status = STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY;

    if (Found)
        return STATUS_SUCCESS;

    return NT_SUCCESS(Status) ? STATUS_INVALID_PARAMETER : Status;
}
