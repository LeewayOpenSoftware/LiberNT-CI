/* Kernelbase-owned environment aliases and performance counters. */

#include <windows.h>
#include <winternl.h>

BOOL WINAPI QueryPerformanceCounter(PLARGE_INTEGER PerformanceCount)
{
    LARGE_INTEGER frequency;
    NTSTATUS status = NtQueryPerformanceCounter(PerformanceCount, &frequency);

    if (!NT_SUCCESS(status) || !frequency.QuadPart)
    {
        SetLastError(RtlNtStatusToDosError(status ? status : STATUS_NOT_IMPLEMENTED));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(PLARGE_INTEGER Frequency)
{
    LARGE_INTEGER counter;
    NTSTATUS status = NtQueryPerformanceCounter(&counter, Frequency);

    if (!NT_SUCCESS(status) || !Frequency->QuadPart)
    {
        SetLastError(RtlNtStatusToDosError(status ? status : STATUS_NOT_IMPLEMENTED));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI FreeEnvironmentStringsA(LPSTR Environment)
{
    return FreeEnvironmentStringsW((LPWSTR)Environment);
}
