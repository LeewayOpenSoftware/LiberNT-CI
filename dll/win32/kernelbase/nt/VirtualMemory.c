/*
 * PROJECT:         ReactOS Win32 Base API (Orosi KernelBase)
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Virtual memory functions
 *                  Moved from kernel32 client/virtmem.c and client/sysinfo.c; kernelbase implements them on
 *                  Windows 11 26100 and kernel32 forwards.
 * COPYRIGHT:       Alex Ionescu (alex.ionescu@reactos.org)
 *                  Emanuele Aliberti, Christoph von Wittich, Thomas Weidenmueller,
 *                  Gunnar Andre Dalsnes, Stanislav Motylkov, Mark Jansen, Ratin Gao
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

NTSTATUS NTAPI RtlRegisterCfgTargetRange(PVOID Base, SIZE_T Size);
VOID NTAPI RtlUnregisterCfgTargetRange(PVOID Base);

/* PRIVATE FUNCTIONS **********************************************************/

/* kernel32 reads this from the base server's static data; kernelbase asks the kernel once. */
static
ULONG
BaseAllocationGranularity(VOID)
{
    static ULONG Granularity;
    SYSTEM_BASIC_INFORMATION BasicInfo;

    if (Granularity == 0 &&
        NT_SUCCESS(NtQuerySystemInformation(SystemBasicInformation, &BasicInfo, sizeof(BasicInfo), NULL)))
    {
        Granularity = BasicInfo.AllocationGranularity;
    }
    return Granularity;
}

static
BOOLEAN
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

static
BOOLEAN
BasepFlushSecureMemoryCache(
    _In_ HANDLE hProcess,
    _In_ NTSTATUS Status,
    _In_ PVOID BaseAddress,
    _In_ SIZE_T RegionSize)
{
    if (hProcess != GetCurrentProcess())
        return FALSE;

    if (Status != STATUS_INVALID_PAGE_PROTECTION &&
        Status != STATUS_UNABLE_TO_FREE_VM &&
        Status != STATUS_UNABLE_TO_DECOMMIT_VM)
    {
        return FALSE;
    }

    return RtlFlushSecureMemoryCache(BaseAddress, RegionSize);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
LPVOID
NTAPI
VirtualAllocEx(IN HANDLE hProcess,
               IN LPVOID lpAddress,
               IN SIZE_T dwSize,
               IN DWORD flAllocationType,
               IN DWORD flProtect)
{
    NTSTATUS Status;

    /* Make sure the address is within the granularity of the system (64K) */
    if ((lpAddress != NULL) &&
        (lpAddress < UlongToPtr(BaseAllocationGranularity())))
    {
        /* Fail the call */
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    /* Allocate the memory */
    Status = NtAllocateVirtualMemory(hProcess,
                                     &lpAddress,
                                     0,
                                     &dwSize,
                                     flAllocationType,
                                     flProtect);

    /* Check for status */
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return NULL;
    }

    if ((flProtect & PAGE_TARGETS_INVALID) && hProcess == GetCurrentProcess())
    {
        Status = RtlRegisterCfgTargetRange(lpAddress, dwSize);
        if (!NT_SUCCESS(Status))
        {
            SIZE_T FreeSize = 0;
            NtFreeVirtualMemory(hProcess, &lpAddress, &FreeSize, MEM_RELEASE);
            BaseSetLastNTError(Status);
            return NULL;
        }
    }

    /* Return the allocated address */
    return lpAddress;
}

/*
 * @implemented
 */
LPVOID
NTAPI
VirtualAlloc(IN LPVOID lpAddress,
             IN SIZE_T dwSize,
             IN DWORD flAllocationType,
             IN DWORD flProtect)
{
    /* Call the extended API */
    return VirtualAllocEx(GetCurrentProcess(),
                          lpAddress,
                          dwSize,
                          flAllocationType,
                          flProtect);
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualFreeEx(IN HANDLE hProcess,
              IN LPVOID lpAddress,
              IN SIZE_T dwSize,
              IN DWORD dwFreeType)
{
    NTSTATUS Status;
    PVOID ReleasedAddress = lpAddress;
    SIZE_T RequestedSize = dwSize;

    /* Validate size and flags */
    if (!(dwSize) || (dwFreeType != MEM_RELEASE))
    {
        /* Free the memory */
        Status = NtFreeVirtualMemory(hProcess,
                                     &lpAddress,
                                     &dwSize,
                                     dwFreeType);
        if (!NT_SUCCESS(Status) &&
            BasepFlushSecureMemoryCache(hProcess, Status, ReleasedAddress, RequestedSize))
        {
            lpAddress = ReleasedAddress;
            dwSize = RequestedSize;
            Status = NtFreeVirtualMemory(hProcess,
                                         &lpAddress,
                                         &dwSize,
                                         dwFreeType);
        }
        if (!NT_SUCCESS(Status))
        {
            /* We failed */
            BaseSetLastNTError(Status);
            return FALSE;
        }

        if ((dwFreeType & MEM_RELEASE) && hProcess == GetCurrentProcess())
            RtlUnregisterCfgTargetRange(ReleasedAddress);

        /* Return success */
        return TRUE;
    }

    /* Invalid combo */
    BaseSetLastNTError(STATUS_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualFree(IN LPVOID lpAddress,
            IN SIZE_T dwSize,
            IN DWORD dwFreeType)
{
    /* Call the extended API */
    return VirtualFreeEx(GetCurrentProcess(),
                         lpAddress,
                         dwSize,
                         dwFreeType);
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualProtect(IN LPVOID lpAddress,
               IN SIZE_T dwSize,
               IN DWORD flNewProtect,
               OUT PDWORD lpflOldProtect)
{
    /* Call the extended API */
    return VirtualProtectEx(GetCurrentProcess(),
                            lpAddress,
                            dwSize,
                            flNewProtect,
                            lpflOldProtect);
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualProtectEx(IN HANDLE hProcess,
                 IN LPVOID lpAddress,
                 IN SIZE_T dwSize,
                 IN DWORD flNewProtect,
                 OUT PDWORD lpflOldProtect)
{
    NTSTATUS Status;
    PVOID BaseAddress = lpAddress;
    SIZE_T RegionSize = dwSize;

    if (lpflOldProtect == NULL)
    {
        BaseSetLastNTError(STATUS_ACCESS_VIOLATION);
        return FALSE;
    }

    /* Change the protection */
    Status = NtProtectVirtualMemory(hProcess,
                                    &lpAddress,
                                    &dwSize,
                                    flNewProtect,
                                    (PULONG)lpflOldProtect);
    if (!NT_SUCCESS(Status) &&
        BasepFlushSecureMemoryCache(hProcess, Status, BaseAddress, RegionSize))
    {
        lpAddress = BaseAddress;
        dwSize = RegionSize;
        Status = NtProtectVirtualMemory(hProcess,
                                        &lpAddress,
                                        &dwSize,
                                        flNewProtect,
                                        (PULONG)lpflOldProtect);
    }
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualLock(IN LPVOID lpAddress,
            IN SIZE_T dwSize)
{
    NTSTATUS Status;
    SIZE_T RegionSize = dwSize;
    PVOID BaseAddress = lpAddress;

    /* Lock the memory */
    Status = NtLockVirtualMemory(NtCurrentProcess(),
                                 &BaseAddress,
                                 &RegionSize,
                                 MAP_PROCESS);
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/*
 * @implemented
 */
SIZE_T
NTAPI
VirtualQuery(IN LPCVOID lpAddress,
             OUT PMEMORY_BASIC_INFORMATION lpBuffer,
             IN SIZE_T dwLength)
{
    /* Call the extended API */
    return VirtualQueryEx(NtCurrentProcess(),
                          lpAddress,
                          lpBuffer,
                          dwLength);
}

/*
 * @implemented
 */
SIZE_T
NTAPI
VirtualQueryEx(IN HANDLE hProcess,
               IN LPCVOID lpAddress,
               OUT PMEMORY_BASIC_INFORMATION lpBuffer,
               IN SIZE_T dwLength)
{
    NTSTATUS Status;
    SIZE_T ResultLength;

    /* Query basic information */
    Status = NtQueryVirtualMemory(hProcess,
                                  (LPVOID)lpAddress,
                                  MemoryBasicInformation,
                                  lpBuffer,
                                  dwLength,
                                  &ResultLength);
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return 0;
    }

    /* Return the length returned */
    return ResultLength;
}

/*
 * @implemented
 */
BOOL
NTAPI
VirtualUnlock(IN LPVOID lpAddress,
              IN SIZE_T dwSize)
{
    NTSTATUS Status;
    SIZE_T RegionSize = dwSize;
    PVOID BaseAddress = lpAddress;

    /* Lock the memory */
    Status = NtUnlockVirtualMemory(NtCurrentProcess(),
                                   &BaseAddress,
                                   &RegionSize,
                                   MAP_PROCESS);
    if (!NT_SUCCESS(Status))
    {
        /* We failed */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Return success */
    return TRUE;
}

/*
 * @implemented
 */
UINT
WINAPI
GetWriteWatch(IN DWORD dwFlags,
              IN PVOID lpBaseAddress,
              IN SIZE_T dwRegionSize,
              IN PVOID *lpAddresses,
              OUT PULONG_PTR lpdwCount,
              OUT PULONG lpdwGranularity)
{
    NTSTATUS Status;

    Status = NtGetWriteWatch(GetCurrentProcess(),
                             dwFlags,
                             lpBaseAddress,
                             dwRegionSize,
                             lpAddresses,
                             lpdwCount,
                             lpdwGranularity);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return -1;
    }

    return 0;
}

/*
 * @implemented
 */
UINT
WINAPI
ResetWriteWatch(IN LPVOID lpBaseAddress,
                IN SIZE_T dwRegionSize)
{
    NTSTATUS Status;

    Status = NtResetWriteWatch(NtCurrentProcess(),
                               lpBaseAddress,
                               dwRegionSize);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return -1;
    }

    return 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
AllocateUserPhysicalPages(IN HANDLE hProcess,
                          IN PULONG_PTR NumberOfPages,
                          OUT PULONG_PTR UserPfnArray)
{
    NTSTATUS Status;

    Status = NtAllocateUserPhysicalPages(hProcess, NumberOfPages, UserPfnArray);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

BOOL
WINAPI
AllocateUserPhysicalPagesNuma(IN HANDLE hProcess,
                              IN OUT PULONG_PTR NumberOfPages,
                              OUT PULONG_PTR PageArray,
                              IN DWORD nndPreferred)
{
    if ((nndPreferred != NUMA_NO_PREFERRED_NODE) && !BasepIsValidNumaNode(nndPreferred))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* TODO: nndPreferred does not steer the frame choice; any node is used */
    return AllocateUserPhysicalPages(hProcess, NumberOfPages, PageArray);
}

/*
 * @implemented
 */
BOOL
WINAPI
FreeUserPhysicalPages(IN HANDLE hProcess,
                      IN PULONG_PTR NumberOfPages,
                      IN PULONG_PTR PageArray)
{
    NTSTATUS Status;

    Status = NtFreeUserPhysicalPages(hProcess, NumberOfPages, PageArray);
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
SIZE_T
WINAPI
GetLargePageMinimum(VOID)
{
    return SharedUserData->LargePageMinimum;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemFileCacheSize(OUT PSIZE_T lpMinimumFileCacheSize,
                       OUT PSIZE_T lpMaximumFileCacheSize,
                       OUT PDWORD lpFlags)
{
    SYSTEM_FILECACHE_INFORMATION CacheInfo;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemFileCacheInformationEx,
                                      &CacheInfo,
                                      sizeof(CacheInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    *lpMinimumFileCacheSize = CacheInfo.MinimumWorkingSet;
    *lpMaximumFileCacheSize = CacheInfo.MaximumWorkingSet;
    *lpFlags = CacheInfo.Flags;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetSystemFileCacheSize(IN SIZE_T MinimumFileCacheSize,
                       IN SIZE_T MaximumFileCacheSize,
                       IN DWORD Flags)
{
    SYSTEM_FILECACHE_INFORMATION CacheInfo;
    NTSTATUS Status;

    if ((Flags & ~(FILE_CACHE_MAX_HARD_ENABLE | FILE_CACHE_MAX_HARD_DISABLE |
                   FILE_CACHE_MIN_HARD_ENABLE | FILE_CACHE_MIN_HARD_DISABLE)) ||
        ((Flags & FILE_CACHE_MAX_HARD_ENABLE) && (Flags & FILE_CACHE_MAX_HARD_DISABLE)) ||
        ((Flags & FILE_CACHE_MIN_HARD_ENABLE) && (Flags & FILE_CACHE_MIN_HARD_DISABLE)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(&CacheInfo, sizeof(CacheInfo));
    CacheInfo.MinimumWorkingSet = MinimumFileCacheSize;
    CacheInfo.MaximumWorkingSet = MaximumFileCacheSize;
    CacheInfo.Flags = Flags;

    Status = NtSetSystemInformation(SystemFileCacheInformationEx,
                                    &CacheInfo,
                                    sizeof(CacheInfo));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
/* EOF */
