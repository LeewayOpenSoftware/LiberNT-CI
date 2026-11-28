/*
 * PROJECT:         ReactOS Win32 Base API (Orosi KernelBase)
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         System information functions
 *                  Moved from kernel32 client/sysinfo.c; kernelbase implements them on
 *                  Windows 11 26100 and kernel32 forwards.
 * COPYRIGHT:       Emanuele Aliberti
 *                  Christoph von Wittich
 *                  Thomas Weidenmueller
 *                  Gunnar Andre Dalsnes
 *                  Stanislav Motylkov (x86corez@gmail.com)
 *                  Mark Jansen (mark.jansen@reactos.org)
 *                  Copyright 2023 Ratin Gao <ratin@knsoft.org>
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

#define PV_NT351 0x00030033

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
GetSystemInfoInternal(IN PSYSTEM_BASIC_INFORMATION BasicInfo,
                      IN PSYSTEM_PROCESSOR_INFORMATION ProcInfo,
                      OUT LPSYSTEM_INFO SystemInfo)
{
    RtlZeroMemory(SystemInfo, sizeof (SYSTEM_INFO));
    SystemInfo->wProcessorArchitecture = ProcInfo->ProcessorArchitecture;
    SystemInfo->wReserved = 0;
    SystemInfo->dwPageSize = BasicInfo->PageSize;
    SystemInfo->lpMinimumApplicationAddress = (PVOID)BasicInfo->MinimumUserModeAddress;
    SystemInfo->lpMaximumApplicationAddress = (PVOID)BasicInfo->MaximumUserModeAddress;
    SystemInfo->dwActiveProcessorMask = BasicInfo->ActiveProcessorsAffinityMask;
    SystemInfo->dwNumberOfProcessors = BasicInfo->NumberOfProcessors;
    SystemInfo->wProcessorLevel = ProcInfo->ProcessorLevel;
    SystemInfo->wProcessorRevision = ProcInfo->ProcessorRevision;
    SystemInfo->dwAllocationGranularity = BasicInfo->AllocationGranularity;

    switch (ProcInfo->ProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_INTEL:
            switch (ProcInfo->ProcessorLevel)
            {
                case 3:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_386;
                    break;
                case 4:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_486;
                    break;
                default:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
            }
            break;

        case PROCESSOR_ARCHITECTURE_AMD64:
            SystemInfo->dwProcessorType = PROCESSOR_AMD_X8664;
            break;

        case PROCESSOR_ARCHITECTURE_IA64:
            SystemInfo->dwProcessorType = PROCESSOR_INTEL_IA64;
            break;

        default:
            SystemInfo->dwProcessorType = 0;
            break;
    }

    if (PV_NT351 > GetProcessVersion(0))
    {
        SystemInfo->wProcessorLevel = 0;
        SystemInfo->wProcessorRevision = 0;
    }
}

static
UINT
BaseQuerySystemFirmware(
    _In_ DWORD FirmwareTableProviderSignature,
    _In_ DWORD FirmwareTableID,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableBuffer,
    _In_ DWORD BufferSize,
    _In_ SYSTEM_FIRMWARE_TABLE_ACTION Action)
{
    SYSTEM_FIRMWARE_TABLE_INFORMATION* SysFirmwareInfo;
    _SEH2_VOLATILE ULONG Result = 0;
    ULONG ReturnedSize;
    /* The trailing array and structure padding are not caller buffer space. */
    ULONG TotalSize = BufferSize + FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
    _SEH2_VOLATILE NTSTATUS Status = STATUS_UNSUCCESSFUL;

    SysFirmwareInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, TotalSize);
    if (!SysFirmwareInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    SysFirmwareInfo->ProviderSignature = FirmwareTableProviderSignature;
    SysFirmwareInfo->TableID = FirmwareTableID;
    SysFirmwareInfo->Action = Action;
    SysFirmwareInfo->TableBufferLength = BufferSize;

    Status = NtQuerySystemInformation(SystemFirmwareTableInformation, SysFirmwareInfo, TotalSize, &ReturnedSize);

    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_TOO_SMALL)
        Result = SysFirmwareInfo->TableBufferLength;

    if (NT_SUCCESS(Status) && pFirmwareTableBuffer)
    {
        _SEH2_TRY
        {
            RtlCopyMemory(pFirmwareTableBuffer, SysFirmwareInfo->TableBuffer, SysFirmwareInfo->TableBufferLength);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
            Result = 0;
        }
        _SEH2_END;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, SysFirmwareInfo);

    BaseSetLastNTError(Status);
    return Result;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
VOID
WINAPI
GetSystemInfo(IN LPSYSTEM_INFO lpSystemInfo)
{
    SYSTEM_BASIC_INFORMATION BasicInfo;
    SYSTEM_PROCESSOR_INFORMATION ProcInfo;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemBasicInformation,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      0);
    if (!NT_SUCCESS(Status)) return;

    Status = NtQuerySystemInformation(SystemProcessorInformation,
                                      &ProcInfo,
                                      sizeof(ProcInfo),
                                      0);
    if (!NT_SUCCESS(Status)) return;

    GetSystemInfoInternal(&BasicInfo, &ProcInfo, lpSystemInfo);
}

/*
 * @implemented
 */
VOID
WINAPI
GetNativeSystemInfo(IN LPSYSTEM_INFO lpSystemInfo)
{
    SYSTEM_BASIC_INFORMATION BasicInfo;
    SYSTEM_PROCESSOR_INFORMATION ProcInfo;
    SYSTEM_INFO ProcessInfo;
    SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION Machines[8];
    HANDLE Process = NULL;
    ULONG Index;
    NTSTATUS Status;

    Status = RtlGetNativeSystemInformation(SystemBasicInformation,
                                           &BasicInfo,
                                           sizeof(BasicInfo),
                                           0);
    if (!NT_SUCCESS(Status)) return;

    Status = RtlGetNativeSystemInformation(SystemProcessorInformation,
                                           &ProcInfo,
                                           sizeof(ProcInfo),
                                           0);
    if (!NT_SUCCESS(Status)) return;

    GetSystemInfoInternal(&BasicInfo, &ProcInfo, lpSystemInfo);

    if (ProcInfo.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ||
        ProcInfo.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        ProcInfo.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
    {
        return;
    }

    RtlZeroMemory(&ProcessInfo, sizeof(ProcessInfo));
    ProcessInfo.wProcessorArchitecture = PROCESSOR_ARCHITECTURE_UNKNOWN;
    GetSystemInfo(&ProcessInfo);
    if (ProcessInfo.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL &&
        ProcessInfo.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
    {
        return;
    }

    *lpSystemInfo = ProcessInfo;
    if (ProcessInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        return;

    RtlZeroMemory(Machines, sizeof(Machines));
    Status = NtQuerySystemInformationEx(SystemSupportedProcessorArchitectures2,
                                        &Process, sizeof(Process),
                                        Machines, sizeof(Machines), NULL);
    if (!NT_SUCCESS(Status))
        return;

    for (Index = 0; Index < RTL_NUMBER_OF(Machines) && Machines[Index].Machine; ++Index)
    {
        if (Machines[Index].Machine == IMAGE_FILE_MACHINE_AMD64 && Machines[Index].UserMode)
        {
            lpSystemInfo->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
            break;
        }
    }
}

BOOL
WINAPI
GetPhysicallyInstalledSystemMemory(PULONGLONG TotalMemoryInKilobytes)
{
    PUCHAR Buffer, Entry, End, Strings;
    DWORD BufferSize, ReturnedSize, TableSize, ExtendedSize;
    DWORD Error = ERROR_INVALID_DATA;
    USHORT DeviceSize;
    ULONGLONG TotalMemory = 0, DeviceMemory;
    MEMORYSTATUSEX MemoryStatus;
    BOOL Result = FALSE;

    if (!TotalMemoryInKilobytes)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    BufferSize = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (!BufferSize)
        return FALSE;
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (!Buffer)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    ReturnedSize = GetSystemFirmwareTable('RSMB', 0, Buffer, BufferSize);
    if (!ReturnedSize)
    {
        Error = GetLastError();
        goto Exit;
    }
    if (ReturnedSize > BufferSize || ReturnedSize < 8)
        goto Exit;
    RtlCopyMemory(&TableSize, Buffer + 4, sizeof(TableSize));
    if (TableSize > ReturnedSize - 8)
        goto Exit;
    Entry = Buffer + 8;
    End = Entry + TableSize;
    while (Entry < End)
    {
        if (End - Entry < 4 || Entry[1] < 4 || Entry[1] > End - Entry)
            goto Exit;
        Strings = Entry + Entry[1];
        while (End - Strings >= 2 && (Strings[0] || Strings[1]))
            ++Strings;
        if (End - Strings < 2)
            goto Exit;
        if (Entry[0] == 127)
            break;
        if (Entry[0] == 17)
        {
            if (Entry[1] < 0x0e)
                goto Exit;
            RtlCopyMemory(&DeviceSize, Entry + 0x0c, sizeof(DeviceSize));
            if (DeviceSize == 0xffff)
                goto Exit;
            if (DeviceSize == 0x7fff)
            {
                if (Entry[1] < 0x20)
                    goto Exit;
                RtlCopyMemory(&ExtendedSize, Entry + 0x1c, sizeof(ExtendedSize));
                if (ExtendedSize & 0x80000000)
                    goto Exit;
                DeviceMemory = (ULONGLONG)ExtendedSize * 1024;
            }
            else if (DeviceSize & 0x8000)
            {
                DeviceMemory = DeviceSize & 0x7fff;
            }
            else
            {
                DeviceMemory = (ULONGLONG)DeviceSize * 1024;
            }
            if (DeviceMemory > MAXULONGLONG - TotalMemory)
                goto Exit;
            TotalMemory += DeviceMemory;
        }
        Entry = Strings + 2;
    }
    MemoryStatus.dwLength = sizeof(MemoryStatus);
    if (!GlobalMemoryStatusEx(&MemoryStatus))
    {
        Error = GetLastError();
        goto Exit;
    }
    if (!TotalMemory || TotalMemory < MemoryStatus.ullTotalPhys / 1024)
        goto Exit;
    *TotalMemoryInKilobytes = TotalMemory;
    Result = TRUE;

Exit:
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    if (!Result)
        SetLastError(Error);
    return Result;
}

/**
 * @name EnumSystemFirmwareTables
 * @implemented
 *
 * Obtains firmware table identifiers.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-enumsystemfirmwaretables
 *
 * @param FirmwareTableProviderSignature
 * Can be either ACPI, FIRM, or RSMB.
 *
 * @param pFirmwareTableBuffer
 * Pointer to the output buffer, can be NULL.
 *
 * @param BufferSize
 * Size of the output buffer.
 *
 * @return
 * Actual size of the data in case of success, 0 otherwise.
 *
 * @remarks
 * Data would be written to buffer only if the specified size is
 * larger or equal to the actual size, in the other case Last Error
 * value would be set to ERROR_INSUFFICIENT_BUFFER.
 * In case of incorrect provider signature, Last Error value would be
 * set to ERROR_INVALID_FUNCTION.
 *
 */
UINT
WINAPI
EnumSystemFirmwareTables(
    _In_ DWORD FirmwareTableProviderSignature,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableEnumBuffer,
    _In_ DWORD BufferSize)
{
    return BaseQuerySystemFirmware(FirmwareTableProviderSignature,
                                   0,
                                   pFirmwareTableEnumBuffer,
                                   BufferSize,
                                   SystemFirmwareTable_Enumerate);
}

/**
 * @name GetSystemFirmwareTable
 * @implemented
 *
 * Obtains the firmware table data.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemfirmwaretable
 *
 * @param FirmwareTableProviderSignature
 * Can be either ACPI, FIRM, or RSMB.
 *
 * @param FirmwareTableID
 * Correct table identifier.
 *
 * @param pFirmwareTableBuffer
 * Pointer to the output buffer, can be NULL.
 *
 * @param BufferSize
 * Size of the output buffer.
 *
 * @return
 * Actual size of the data in case of success, 0 otherwise.
 *
 * @remarks
 * Data would be written to buffer only if the specified size is
 * larger or equal to the actual size, in the other case Last Error
 * value would be set to ERROR_INSUFFICIENT_BUFFER.
 * In case of incorrect provider signature, Last Error value would be
 * set to ERROR_INVALID_FUNCTION.
 * Also Last Error value becomes ERROR_NOT_FOUND if incorrect
 * table identifier was specified along with ACPI provider, and
 * ERROR_INVALID_PARAMETER along with FIRM provider. The RSMB provider
 * accepts any table identifier.
 *
 */
UINT
WINAPI
GetSystemFirmwareTable(
    _In_ DWORD FirmwareTableProviderSignature,
    _In_ DWORD FirmwareTableID,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableBuffer,
    _In_ DWORD BufferSize)
{
    return BaseQuerySystemFirmware(FirmwareTableProviderSignature,
                                   FirmwareTableID,
                                   pFirmwareTableBuffer,
                                   BufferSize,
                                   SystemFirmwareTable_Get);
}
/* EOF */
