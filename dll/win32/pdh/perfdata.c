/*
 * Performance Data Helpers - Shared measurement code for PDH and perflib providers
 * Used by both PDH counter callbacks and perflib provider DLLs (PerfOS, PerfProc)
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <stdlib.h>
#include "windef.h"
#include "winbase.h"
#include <winperf.h>
#include <psapi.h>
#include "winternl.h"

#define NDEBUG
#include <debug.h>

/* Get processor performance data (time-based) */
NTSTATUS perf_get_processor_time(
    ULONGLONG *kernel_time,
    ULONGLONG *user_time,
    ULONGLONG *idle_time)
{
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION *performance;
    SYSTEM_INFO info;
    ULONG count, returned = 0;
    NTSTATUS status;
    ULONGLONG total_kernel = 0, total_user = 0, total_idle = 0;
    int i;

    GetSystemInfo(&info);
    count = info.dwNumberOfProcessors;
    if (!count)
        return STATUS_INSUFFICIENT_RESOURCES;

    performance = malloc(count * sizeof(*performance));
    if (!performance)
        return STATUS_NO_MEMORY;

    status = NtQuerySystemInformation(
        SystemProcessorPerformanceInformation,
        performance,
        count * sizeof(*performance),
        &returned);

    if (status >= 0)
    {
        for (i = 0; i < count; i++)
        {
            total_kernel += performance[i].KernelTime.QuadPart;
            total_user += performance[i].UserTime.QuadPart;
            total_idle += performance[i].IdleTime.QuadPart;
        }
        *kernel_time = total_kernel;
        *user_time = total_user;
        *idle_time = total_idle;
    }

    free(performance);
    return status;
}

/* Get available memory in pages */
NTSTATUS perf_get_available_memory(ULONGLONG *available_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *available_pages = perf_info.PhysicalAvailable;
    return STATUS_SUCCESS;
}

/* Get committed memory in pages */
NTSTATUS perf_get_committed_memory(ULONGLONG *committed_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *committed_pages = perf_info.CommitTotal;
    return STATUS_SUCCESS;
}

/* Get total physical memory */
NTSTATUS perf_get_total_memory(ULONGLONG *total_pages)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *total_pages = perf_info.PhysicalTotal;
    return STATUS_SUCCESS;
}

/* Get process count */
NTSTATUS perf_get_process_count(ULONG *count)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *count = perf_info.ProcessCount;
    return STATUS_SUCCESS;
}

/* Get thread count */
NTSTATUS perf_get_thread_count(ULONG *count)
{
    PERFORMANCE_INFORMATION perf_info;

    if (!GetPerformanceInfo(&perf_info, sizeof(perf_info)))
        return STATUS_UNSUCCESSFUL;

    *count = perf_info.ThreadCount;
    return STATUS_SUCCESS;
}
