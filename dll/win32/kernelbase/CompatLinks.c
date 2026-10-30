/*
 * PROJECT:     LiberNT KernelBase
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     KernelBase exports whose old target was a kernel32 stub. Each one
 *              either does its documented job or fails with an explicit error
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <winternl.h>

BOOL WINAPI GetNumaHighestNodeNumber(_Out_ PULONG HighestNodeNumber);

#ifndef NUMA_NO_PREFERRED_NODE
#define NUMA_NO_PREFERRED_NODE ((DWORD)-1)
#endif

/* The preferred node of the NUMA allocators must be "none" or a node that exists */
static
BOOL
IsUsablePreferredNode(
    _In_ DWORD Node)
{
    ULONG Highest;

    if (Node == NUMA_NO_PREFERRED_NODE)
        return TRUE;

    if (!GetNumaHighestNodeNumber(&Highest))
        return FALSE;

    if (Node > Highest)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return TRUE;
}

LPVOID
WINAPI
VirtualAllocExNuma(
    _In_ HANDLE Process,
    _In_opt_ LPVOID Address,
    _In_ SIZE_T Size,
    _In_ DWORD AllocationType,
    _In_ DWORD Protect,
    _In_ DWORD PreferredNode)
{
    if (!IsUsablePreferredNode(PreferredNode))
        return NULL;

    /* A node that exists is accepted, but the allocation is not steered to it */
    return VirtualAllocEx(Process, Address, Size, AllocationType, Protect);
}

LPVOID
WINAPI
MapViewOfFileExNuma(
    _In_ HANDLE FileMapping,
    _In_ DWORD DesiredAccess,
    _In_ DWORD FileOffsetHigh,
    _In_ DWORD FileOffsetLow,
    _In_ SIZE_T NumberOfBytesToMap,
    _In_opt_ LPVOID BaseAddress,
    _In_ DWORD PreferredNode)
{
    if (!IsUsablePreferredNode(PreferredNode))
        return NULL;

    return MapViewOfFileEx(FileMapping,
                           DesiredAccess,
                           FileOffsetHigh,
                           FileOffsetLow,
                           NumberOfBytesToMap,
                           BaseAddress);
}

BOOL
WINAPI
SetStdHandleEx(
    _In_ DWORD StdHandle,
    _In_ HANDLE Handle,
    _Out_opt_ PHANDLE PreviousHandle)
{
    HANDLE Previous;

    if (StdHandle != STD_INPUT_HANDLE &&
        StdHandle != STD_OUTPUT_HANDLE &&
        StdHandle != STD_ERROR_HANDLE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    Previous = GetStdHandle(StdHandle);
    if (!SetStdHandle(StdHandle, Handle))
        return FALSE;

    if (PreviousHandle)
        *PreviousHandle = Previous;

    return TRUE;
}

/* Processor-group affinity is not modelled: report it instead of pretending */
BOOL
WINAPI
SetProcessAffinityUpdateMode(
    _In_ HANDLE Process,
    _In_ DWORD Flags)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL
WINAPI
QueryProcessAffinityUpdateMode(
    _In_ HANDLE Process,
    _Out_opt_ LPDWORD Flags)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* Named pipes keep no client computer name */
BOOL
WINAPI
GetNamedPipeClientComputerNameW(
    _In_ HANDLE Pipe,
    _Out_writes_bytes_(ClientComputerNameLength) LPWSTR ClientComputerName,
    _In_ ULONG ClientComputerNameLength)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* Working-set watching is not provided by the kernel: report it */
BOOL
WINAPI
GetWsChangesEx(
    _In_ HANDLE Process,
    _Out_writes_bytes_to_(*Size, *Size) PVOID Info,
    _Inout_ PDWORD Size)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

HANDLE
WINAPI
CreateFileMappingNumaW(
    _In_ HANDLE File,
    _In_opt_ LPSECURITY_ATTRIBUTES Attributes,
    _In_ DWORD Protect,
    _In_ DWORD MaximumSizeHigh,
    _In_ DWORD MaximumSizeLow,
    _In_opt_ LPCWSTR Name,
    _In_ DWORD PreferredNode)
{
    if (!IsUsablePreferredNode(PreferredNode))
        return NULL;

    /* A node that exists is accepted, but the section is not steered to it */
    return CreateFileMappingW(File, Attributes, Protect, MaximumSizeHigh, MaximumSizeLow, Name);
}

HANDLE
WINAPI
CreateFileMappingNumaA(
    _In_ HANDLE File,
    _In_opt_ LPSECURITY_ATTRIBUTES Attributes,
    _In_ DWORD Protect,
    _In_ DWORD MaximumSizeHigh,
    _In_ DWORD MaximumSizeLow,
    _In_opt_ LPCSTR Name,
    _In_ DWORD PreferredNode)
{
    if (!IsUsablePreferredNode(PreferredNode))
        return NULL;

    return CreateFileMappingA(File, Attributes, Protect, MaximumSizeHigh, MaximumSizeLow, Name);
}
