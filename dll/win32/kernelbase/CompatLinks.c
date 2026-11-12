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

NTSYSAPI
NTSTATUS
NTAPI
NtAllocateVirtualMemoryEx(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG AllocationType,
    _In_ ULONG PageProtection,
    _Inout_updates_opt_(ExtendedParameterCount) PMEM_EXTENDED_PARAMETER ExtendedParameters,
    _In_ ULONG ExtendedParameterCount);

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
    MEM_EXTENDED_PARAMETER NumaNode;
    PVOID Base = Address;
    NTSTATUS Status;

    /* No preferred node: the plain allocator */
    if (PreferredNode == NUMA_NO_PREFERRED_NODE)
        return VirtualAllocEx(Process, Address, Size, AllocationType, Protect);

    /* The memory manager takes the node: it checks the process handle first
     * and then refuses a node that does not exist */
    RtlZeroMemory(&NumaNode, sizeof(NumaNode));
    NumaNode.Type = MemExtendedParameterNumaNode;
    NumaNode.ULong = PreferredNode;

    Status = NtAllocateVirtualMemoryEx(Process, &Base, &Size, AllocationType, Protect, &NumaNode, 1);
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return NULL;
    }

    return Base;
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

/* The ANSI form reports exactly what the wide form reports, converted */
BOOL
WINAPI
GetNamedPipeClientComputerNameA(
    _In_ HANDLE Pipe,
    _Out_writes_bytes_(ClientComputerNameLength) LPSTR ClientComputerName,
    _In_ ULONG ClientComputerNameLength)
{
    WCHAR Wide[MAX_COMPUTERNAME_LENGTH + 1];

    if (!GetNamedPipeClientComputerNameW(Pipe, Wide, sizeof(Wide)))
        return FALSE;

    if (!WideCharToMultiByte(CP_ACP, 0, Wide, -1, ClientComputerName,
                             ClientComputerNameLength, NULL, NULL))
    {
        return FALSE;
    }

    return TRUE;
}

/*
 * I/O rings are not provided (Windows 11 feature). The export is kept, as the
 * import library lists it, but it reports E_NOTIMPL-equivalent failure with
 * the capabilities cleared so that a caller never sees a version or
 * operation it could try to use.
 */
HRESULT
WINAPI
QueryIoRingCapabilities(
    _Out_ PVOID Capabilities)
{
    if (!Capabilities)
        return E_POINTER;

    /* IORING_CAPABILITIES is { IORING_VERSION MaxVersion; ULONG MaxSubmissionQueueSize;
     * ULONG MaxCompletionQueueSize; IORING_FEATURE_FLAGS FeatureFlags; } = 16 bytes */
    RtlZeroMemory(Capabilities, 16);
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
