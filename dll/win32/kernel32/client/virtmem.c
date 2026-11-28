/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/virtmem.c
 * PURPOSE:         Handles virtual memory APIs
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

NTSTATUS NTAPI RtlRegisterCfgTargetRange(PVOID Base, SIZE_T Size);
VOID NTAPI RtlUnregisterCfgTargetRange(PVOID Base);

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
MapUserPhysicalPages(IN PVOID VirtualAddress,
                     IN ULONG_PTR NumberOfPages,
                     OUT PULONG_PTR PageArray OPTIONAL)
{
    NTSTATUS Status;

    Status = NtMapUserPhysicalPages(VirtualAddress, NumberOfPages, PageArray);
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
MapUserPhysicalPagesScatter(IN PVOID *VirtualAddresses,
                            IN ULONG_PTR NumberOfPages,
                            OUT PULONG_PTR PageArray OPTIONAL)
{
    NTSTATUS Status;

    Status = NtMapUserPhysicalPagesScatter(VirtualAddresses,
                                           NumberOfPages,
                                           PageArray);
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
AddSecureMemoryCacheCallback(
    _In_ PSECURE_MEMORY_CACHE_CALLBACK pfnCallBack)
{
    NTSTATUS Status;

    Status = RtlRegisterSecureMemoryCacheCallback((PRTL_SECURE_MEMORY_CACHE_CALLBACK)pfnCallBack);
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
RemoveSecureMemoryCacheCallback(
    _In_ PSECURE_MEMORY_CACHE_CALLBACK pfnCallBack)
{
    NTSTATUS Status;

    Status = RtlDeregisterSecureMemoryCacheCallback((PRTL_SECURE_MEMORY_CACHE_CALLBACK)pfnCallBack);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/* EOF */
