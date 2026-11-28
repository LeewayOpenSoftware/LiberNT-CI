/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         System Information Functions
 * COPYRIGHT:       Emanuele Aliberti
 *                  Christoph von Wittich
 *                  Thomas Weidenmueller
 *                  Gunnar Andre Dalsnes
 *                  Stanislav Motylkov (x86corez@gmail.com)
 *                  Mark Jansen (mark.jansen@reactos.org)
 *                  Copyright 2023 Ratin Gao <ratin@knsoft.org>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

#define PV_NT351 0x00030033

/* PRIVATE FUNCTIONS **********************************************************/

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
IsProcessorFeaturePresent(IN DWORD ProcessorFeature)
{
#if defined(_M_ARM64EC) || (defined(_M_IX86) && DLL_EXPORT_VERSION >= 0xA00)
    return RtlIsProcessorFeaturePresent(ProcessorFeature);
#else
    if (ProcessorFeature >= PROCESSOR_FEATURE_MAX) return FALSE;
    return ((BOOL)SharedUserData->ProcessorFeatures[ProcessorFeature]);
#endif
}

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemRegistryQuota(OUT PDWORD pdwQuotaAllowed,
                       OUT PDWORD pdwQuotaUsed)
{
    SYSTEM_REGISTRY_QUOTA_INFORMATION QuotaInfo;
    ULONG BytesWritten;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemRegistryQuotaInformation,
                                      &QuotaInfo,
                                      sizeof(QuotaInfo),
                                      &BytesWritten);
    if (NT_SUCCESS(Status))
    {
      if (pdwQuotaAllowed) *pdwQuotaAllowed = QuotaInfo.RegistryQuotaAllowed;
      if (pdwQuotaUsed) *pdwQuotaUsed = QuotaInfo.RegistryQuotaUsed;
      return TRUE;
    }

    BaseSetLastNTError(Status);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetLogicalProcessorInformation(OUT PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
                               IN OUT PDWORD ReturnLength)
{
    NTSTATUS Status;

    if (!ReturnLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation,
                                      Buffer,
                                      *ReturnLength,
                                      ReturnLength);

    /* Normalize the error to what Win32 expects */
    if (Status == STATUS_INFO_LENGTH_MISMATCH) Status = STATUS_BUFFER_TOO_SMALL;
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaHighestNodeNumber(OUT PULONG HighestNodeNumber)
{
    NTSTATUS Status;
    ULONG Length;
    ULONG PartialInfo[2]; // First two members of SYSTEM_NUMA_INFORMATION

    /* Query partial NUMA info */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      PartialInfo,
                                      sizeof(PartialInfo),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (Length < sizeof(ULONG))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* First member of the struct is the highest node number */
    *HighestNodeNumber = PartialInfo[0];
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaNodeProcessorMask(IN UCHAR Node,
                         OUT PULONGLONG ProcessorMask)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Validate input node number */
    if (Node > NumaInformation.HighestNodeNumber)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Return mask for that node */
    *ProcessorMask = NumaInformation.ActiveProcessorsAffinityMask[Node];
    return TRUE;
}

BOOL
WINAPI
GetNumaNodeProcessorMaskEx(IN USHORT Node,
                           OUT PGROUP_AFFINITY ProcessorMask)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;

    Status = NtQuerySystemInformation(SystemNumaProcessorMap, &NumaInformation, sizeof(NumaInformation), NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (Node > NumaInformation.HighestNodeNumber)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(ProcessorMask, sizeof(*ProcessorMask));
    ProcessorMask->Mask = (KAFFINITY)NumaInformation.ActiveProcessorsAffinityMask[Node];
    ProcessorMask->Group = 0;
    return TRUE;
}

BOOL
WINAPI
GetNumaNodeProcessorMask2(IN USHORT NodeNumber,
                          OUT PGROUP_AFFINITY ProcessorMasks,
                          IN USHORT ProcessorMaskCount,
                          OUT PUSHORT RequiredMaskCount)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    KAFFINITY Mask;

    Status = NtQuerySystemInformation(SystemNumaProcessorMap, &NumaInformation, sizeof(NumaInformation), NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (NodeNumber > NumaInformation.HighestNodeNumber || NodeNumber >= MAXIMUM_NUMA_NODES)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* TODO: report nodes that span several processor groups; only group 0 exists */
    Mask = (KAFFINITY)NumaInformation.ActiveProcessorsAffinityMask[NodeNumber];
    if (Mask == 0)
    {
        *RequiredMaskCount = 0;
        return TRUE;
    }

    *RequiredMaskCount = 1;
    if (ProcessorMaskCount < 1)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    RtlZeroMemory(&ProcessorMasks[0], sizeof(ProcessorMasks[0]));
    ProcessorMasks[0].Mask = Mask;
    ProcessorMasks[0].Group = 0;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaProcessorNode(IN UCHAR Processor,
                     OUT PUCHAR NodeNumber)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;
    ULONG Node;
    ULONGLONG Proc;

    /* Can't handle processor number >= 32 */
    if (Processor >= MAXIMUM_PROCESSORS)
    {
        *NodeNumber = -1;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        *NodeNumber = -1;
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Find ourselves */
    Node = 0;
    Proc = 1ULL << Processor;
    while ((Proc & NumaInformation.ActiveProcessorsAffinityMask[Node]) == 0ULL)
    {
        ++Node;
        /* Out of options */
        if (Node > NumaInformation.HighestNodeNumber)
        {
            *NodeNumber = -1;
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    /* Return found node */
    *NodeNumber = Node;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaProcessorNodeEx(IN PPROCESSOR_NUMBER Processor, OUT PUSHORT NodeNumber)
{
    SYSTEM_NUMA_INFORMATION Information;
    NTSTATUS Status;
    ULONG Node;
    ULONGLONG Mask;

    /* ReactOS currently exposes one processor group. */
    if (Processor->Group != 0 || Processor->Reserved != 0 || Processor->Number >= 64)
    {
        *NodeNumber = 0xffff;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtQuerySystemInformation(SystemNumaProcessorMap, &Information, sizeof(Information), NULL);
    if (!NT_SUCCESS(Status))
    {
        *NodeNumber = 0xffff;
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Mask = 1ULL << Processor->Number;
    for (Node = 0; Node <= Information.HighestNodeNumber && Node < RTL_NUMBER_OF(Information.ActiveProcessorsAffinityMask); ++Node)
    {
        if (Information.ActiveProcessorsAffinityMask[Node] & Mask)
        {
            *NodeNumber = (USHORT)Node;
            return TRUE;
        }
    }

    *NodeNumber = 0xffff;
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaAvailableMemoryNode(IN UCHAR Node,
                           OUT PULONGLONG AvailableBytes)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaAvailableMemory,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Validate input node number */
    if (Node > NumaInformation.HighestNodeNumber)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Return available memory for that node */
    *AvailableBytes = NumaInformation.AvailableMemory[Node];
    return TRUE;
}

BOOLEAN
WINAPI
BasepIsValidNumaNode(IN ULONG Node)
{
    ULONG HighestNodeNumber;

    if (!NT_SUCCESS(NtQuerySystemInformation(SystemNumaProcessorMap,
                                             &HighestNodeNumber,
                                             sizeof(HighestNodeNumber),
                                             NULL)))
    {
        return FALSE;
    }

    return Node <= HighestNodeNumber;
}

BOOL
WINAPI
GetNumaProximityNodeEx(IN ULONG ProximityId,
                       OUT PUSHORT NodeNumber)
{
    SYSTEM_NUMA_PROXIMITY_MAP ProximityMap;
    NTSTATUS Status;

    ProximityMap.NodeProximityId = ProximityId;
    Status = NtQuerySystemInformation(SystemNumaProximityNodeInformation,
                                      &ProximityMap,
                                      sizeof(ProximityMap),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    *NodeNumber = ProximityMap.NodeNumber;
    return TRUE;
}

BOOL
WINAPI
GetNumaProximityNode(IN ULONG ProximityId,
                     OUT PUCHAR NodeNumber)
{
    USHORT Node;

    if (!GetNumaProximityNodeEx(ProximityId, &Node))
        return FALSE;

    *NodeNumber = (UCHAR)Node;
    return TRUE;
}

BOOL
WINAPI
GetNumaNodeNumberFromHandle(IN HANDLE hFile,
                            OUT PUSHORT NodeNumber)
{
    FILE_NUMA_NODE_INFORMATION NumaInformation;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Status = NtQueryInformationFile(hFile,
                                    &IoStatusBlock,
                                    &NumaInformation,
                                    sizeof(NumaInformation),
                                    FileNumaNodeInformation);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    *NodeNumber = NumaInformation.NodeNumber;
    return TRUE;
}

BOOL
WINAPI
GetNumaAvailableMemoryNodeEx(IN USHORT Node,
                             OUT PULONGLONG AvailableBytes)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;

    Status = NtQuerySystemInformation(SystemNumaAvailableMemory,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (Node > NumaInformation.HighestNodeNumber || Node >= MAXIMUM_NUMA_NODES)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *AvailableBytes = NumaInformation.AvailableMemory[Node];
    return TRUE;
}

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableExW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize,
    _Out_opt_ PDWORD pdwAttribubutes);

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableExA(
    _In_ LPCSTR lpName,
    _In_ LPCSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize,
    _Out_opt_ PDWORD pdwAttribubutes);

BOOL
WINAPI
SetFirmwareEnvironmentVariableExW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize,
    _In_ DWORD dwAttributes);

BOOL
WINAPI
SetFirmwareEnvironmentVariableExA(
    _In_ LPCSTR lpName,
    _In_ LPCSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize,
    _In_ DWORD dwAttributes);

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize)
{
    return GetFirmwareEnvironmentVariableExW(lpName, lpGuid, pBuffer, nSize, NULL);
}

BOOL
WINAPI
SetFirmwareEnvironmentVariableW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize)
{
    return SetFirmwareEnvironmentVariableExW(lpName,
                                             lpGuid,
                                             pValue,
                                             nSize,
                                             VARIABLE_ATTRIBUTE_NON_VOLATILE);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetOsSafeBootMode(OUT PDWORD Flags)
{
    if (!Flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *Flags = SharedUserData->SafeBootMode;
    return TRUE;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
OpenStateExplicit(IN PVOID Arg1, OUT PVOID Arg2)
{
    STUB;
    return E_NOTIMPL;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
CloseState(IN PVOID StateHandle)
{
    STUB;
    return E_NOTIMPL;
}

/*
 * @unimplemented
 */
HRESULT
WINAPI
GetStateSettingsFolder(IN PVOID Arg1, IN PVOID Arg2, OUT PVOID Arg3)
{
    STUB;
    return E_NOTIMPL;
}
