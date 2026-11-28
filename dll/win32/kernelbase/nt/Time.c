/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernelbase/nt/Time.c
 * PURPOSE:         Time functions
 *                  Moved from kernel32 client/time.c and
 *                  kernel32_vista/GetTickCount64.c; kernelbase implements
 *                  them on Windows 11 26100 and kernel32 forwards.
 * PROGRAMMER:      Ariadne
 */

/* INCLUDES *******************************************************************/

#include <stdio.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>

#define NTOS_MODE_USER
#include <ndk/cmfuncs.h>
#include <ndk/exfuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>

#include <csr/csr.h>
#include <win/base.h>
#include <win/basemsg.h>
#include <reactos/precisetime.h>

#include <ntstrsafe.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

/* kernel32_shared */
DWORD
WINAPI
BaseSetLastNTError(IN NTSTATUS Status);

/* GLOBALS ********************************************************************/

/* kernel32 keeps this pointer in a global set at attach; kernelbase reads it from the PEB. */
#define BaseStaticServerData \
    ((PBASE_STATIC_SERVER_DATA)NtCurrentPeb()->ReadOnlyStaticServerData[BASESRV_SERVERDLL_INDEX])

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
IsTimeZoneRedirectionEnabled(VOID)
{
    /* Return if a TS Timezone ID is active */
    return (BaseStaticServerData->TermsrvClientTimeZoneId != TIME_ZONE_ID_INVALID);
}

/*
 * @implemented
 */
LONG
WINAPI
CompareFileTime(IN CONST FILETIME *lpFileTime1,
                IN CONST FILETIME *lpFileTime2)
{
    ULARGE_INTEGER Time1, Time2;

    Time1.LowPart = lpFileTime1->dwLowDateTime;
    Time2.LowPart = lpFileTime2->dwLowDateTime;
    Time1.HighPart = lpFileTime1->dwHighDateTime;
    Time2.HighPart = lpFileTime2->dwHighDateTime;

    if (Time1.QuadPart < Time2.QuadPart) return -1;
    if (Time1.QuadPart == Time2.QuadPart) return 0;
    return 1;
}

/*
 * @implemented
 */
VOID
WINAPI
GetSystemTimeAsFileTime(OUT PFILETIME lpFileTime)
{
    LARGE_INTEGER SystemTime;

    SystemTime = KiReadSystemTime(&SharedUserData->SystemTime);

    lpFileTime->dwLowDateTime = SystemTime.LowPart;
    lpFileTime->dwHighDateTime = SystemTime.HighPart;
}

/*
 * @implemented
 */
VOID
WINAPI
GetSystemTimePreciseAsFileTime(OUT PFILETIME lpFileTime)
{
    LARGE_INTEGER SystemTime;

    SystemTime.QuadPart = RtlGetSystemTimePrecise();

    lpFileTime->dwLowDateTime = SystemTime.LowPart;
    lpFileTime->dwHighDateTime = SystemTime.HighPart;
}

/*
 * @implemented
 */
BOOL
WINAPI
SystemTimeToFileTime(IN CONST SYSTEMTIME *lpSystemTime,
                     OUT LPFILETIME lpFileTime)
{
    TIME_FIELDS TimeFields;
    LARGE_INTEGER liTime;

    TimeFields.Year = lpSystemTime->wYear;
    TimeFields.Month = lpSystemTime->wMonth;
    TimeFields.Day = lpSystemTime->wDay;
    TimeFields.Hour = lpSystemTime->wHour;
    TimeFields.Minute = lpSystemTime->wMinute;
    TimeFields.Second = lpSystemTime->wSecond;
    TimeFields.Milliseconds = lpSystemTime->wMilliseconds;

    if (RtlTimeFieldsToTime(&TimeFields, &liTime))
    {
        lpFileTime->dwLowDateTime = liTime.u.LowPart;
        lpFileTime->dwHighDateTime = liTime.u.HighPart;
        return TRUE;
    }

    BaseSetLastNTError(STATUS_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
FileTimeToSystemTime(IN CONST FILETIME *lpFileTime,
                     OUT LPSYSTEMTIME lpSystemTime)
{
    TIME_FIELDS TimeFields;
    LARGE_INTEGER liTime;

    liTime.u.LowPart = lpFileTime->dwLowDateTime;
    liTime.u.HighPart = lpFileTime->dwHighDateTime;
    if (liTime.QuadPart < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlTimeToTimeFields(&liTime, &TimeFields);

    lpSystemTime->wYear = TimeFields.Year;
    lpSystemTime->wMonth = TimeFields.Month;
    lpSystemTime->wDay = TimeFields.Day;
    lpSystemTime->wHour = TimeFields.Hour;
    lpSystemTime->wMinute = TimeFields.Minute;
    lpSystemTime->wSecond = TimeFields.Second;
    lpSystemTime->wMilliseconds = TimeFields.Milliseconds;
    lpSystemTime->wDayOfWeek = TimeFields.Weekday;

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
FileTimeToLocalFileTime(IN CONST FILETIME *lpFileTime,
                        OUT LPFILETIME lpLocalFileTime)
{
    LARGE_INTEGER TimeZoneBias, FileTime;
    volatile KSYSTEM_TIME *TimePtr;

    TimePtr = IsTimeZoneRedirectionEnabled() ?
              &BaseStaticServerData->ktTermsrvClientBias :
              &SharedUserData->TimeZoneBias;

    TimeZoneBias = KiReadSystemTime(TimePtr);

    FileTime.LowPart = lpFileTime->dwLowDateTime;
    FileTime.HighPart = lpFileTime->dwHighDateTime;

    FileTime.QuadPart -= TimeZoneBias.QuadPart;

    lpLocalFileTime->dwLowDateTime = FileTime.LowPart;
    lpLocalFileTime->dwHighDateTime = FileTime.HighPart;

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
LocalFileTimeToFileTime(IN CONST FILETIME *lpLocalFileTime,
                        OUT LPFILETIME lpFileTime)
{
    LARGE_INTEGER TimeZoneBias, FileTime;
    volatile KSYSTEM_TIME *TimePtr;

    TimePtr = IsTimeZoneRedirectionEnabled() ?
              &BaseStaticServerData->ktTermsrvClientBias :
              &SharedUserData->TimeZoneBias;

    TimeZoneBias = KiReadSystemTime(TimePtr);

    FileTime.LowPart = lpLocalFileTime->dwLowDateTime;
    FileTime.HighPart = lpLocalFileTime->dwHighDateTime;

    FileTime.QuadPart += TimeZoneBias.QuadPart;

    lpFileTime->dwLowDateTime = FileTime.LowPart;
    lpFileTime->dwHighDateTime = FileTime.HighPart;

    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
GetLocalTime(OUT LPSYSTEMTIME lpSystemTime)
{
    LARGE_INTEGER SystemTime, TimeZoneBias;
    TIME_FIELDS TimeFields;
    volatile KSYSTEM_TIME *TimePtr;

    SystemTime = KiReadSystemTime(&SharedUserData->SystemTime);

    TimePtr = IsTimeZoneRedirectionEnabled() ?
              &BaseStaticServerData->ktTermsrvClientBias :
              &SharedUserData->TimeZoneBias;

    TimeZoneBias = KiReadSystemTime(TimePtr);

    SystemTime.QuadPart -= TimeZoneBias.QuadPart;
    RtlTimeToTimeFields(&SystemTime, &TimeFields);

    lpSystemTime->wYear = TimeFields.Year;
    lpSystemTime->wMonth = TimeFields.Month;
    lpSystemTime->wDay = TimeFields.Day;
    lpSystemTime->wHour = TimeFields.Hour;
    lpSystemTime->wMinute = TimeFields.Minute;
    lpSystemTime->wSecond = TimeFields.Second;
    lpSystemTime->wMilliseconds = TimeFields.Milliseconds;
    lpSystemTime->wDayOfWeek = TimeFields.Weekday;
}

/*
 * @implemented
 */
VOID
WINAPI
GetSystemTime(OUT LPSYSTEMTIME lpSystemTime)
{
    LARGE_INTEGER SystemTime;
    TIME_FIELDS TimeFields;

    SystemTime = KiReadSystemTime(&SharedUserData->SystemTime);

    RtlTimeToTimeFields(&SystemTime, &TimeFields);

    lpSystemTime->wYear = TimeFields.Year;
    lpSystemTime->wMonth = TimeFields.Month;
    lpSystemTime->wDay = TimeFields.Day;
    lpSystemTime->wHour = TimeFields.Hour;
    lpSystemTime->wMinute = TimeFields.Minute;
    lpSystemTime->wSecond = TimeFields.Second;
    lpSystemTime->wMilliseconds = TimeFields.Milliseconds;
    lpSystemTime->wDayOfWeek = TimeFields.Weekday;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetLocalTime(IN CONST SYSTEMTIME *lpSystemTime)
{
    LARGE_INTEGER NewSystemTime, TimeZoneBias;
    NTSTATUS Status;
    ULONG Privilege = SE_SYSTEMTIME_PRIVILEGE;
    TIME_FIELDS TimeFields;
    PVOID State;
    volatile KSYSTEM_TIME *TimePtr;

    TimePtr = IsTimeZoneRedirectionEnabled() ?
              &BaseStaticServerData->ktTermsrvClientBias :
              &SharedUserData->TimeZoneBias;

    TimeZoneBias = KiReadSystemTime(TimePtr);

    TimeFields.Year = lpSystemTime->wYear;
    TimeFields.Month = lpSystemTime->wMonth;
    TimeFields.Day = lpSystemTime->wDay;
    TimeFields.Hour = lpSystemTime->wHour;
    TimeFields.Minute = lpSystemTime->wMinute;
    TimeFields.Second = lpSystemTime->wSecond;
    TimeFields.Milliseconds = lpSystemTime->wMilliseconds;

    if (!RtlTimeFieldsToTime(&TimeFields, &NewSystemTime))
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    NewSystemTime.QuadPart += TimeZoneBias.QuadPart;

    Status = RtlAcquirePrivilege(&Privilege, 1, 0, &State);
    if (NT_SUCCESS(Status))
    {
        Status = NtSetSystemTime(&NewSystemTime, NULL);
        RtlReleasePrivilege(State);
    }

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
SetSystemTime(IN CONST SYSTEMTIME *lpSystemTime)
{
    LARGE_INTEGER NewSystemTime;
    NTSTATUS Status;
    ULONG Privilege = SE_SYSTEMTIME_PRIVILEGE;
    TIME_FIELDS TimeFields;
    PVOID State;

    TimeFields.Year = lpSystemTime->wYear;
    TimeFields.Month = lpSystemTime->wMonth;
    TimeFields.Day = lpSystemTime->wDay;
    TimeFields.Hour = lpSystemTime->wHour;
    TimeFields.Minute = lpSystemTime->wMinute;
    TimeFields.Second = lpSystemTime->wSecond;
    TimeFields.Milliseconds = lpSystemTime->wMilliseconds;

    if (!RtlTimeFieldsToTime(&TimeFields, &NewSystemTime))
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    Status = RtlAcquirePrivilege(&Privilege, 1, 0, &State);
    if (NT_SUCCESS(Status))
    {
        Status = NtSetSystemTime(&NewSystemTime, NULL);
        RtlReleasePrivilege(State);
    }

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
DWORD
WINAPI
GetTickCount(VOID)
{
    LARGE_INTEGER TickCount;

    TickCount = KiReadSystemTime(&SharedUserData->TickCount);

    /* Convert to milliseconds */
    return (DWORD)KiTickCountToMs(TickCount);
}

/*
 * @implemented
 */
ULONGLONG
WINAPI
GetTickCount64(VOID)
{
    LARGE_INTEGER TickCount;

    TickCount = KiReadSystemTime(&SharedUserData->TickCount);

    /* Convert to milliseconds */
    return KiTickCountToMs(TickCount);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemTimeAdjustment(OUT PDWORD lpTimeAdjustment,
                        OUT PDWORD lpTimeIncrement,
                        OUT PBOOL lpTimeAdjustmentDisabled)
{
    SYSTEM_QUERY_TIME_ADJUST_INFORMATION TimeInfo;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemTimeAdjustmentInformation,
                                      &TimeInfo,
                                      sizeof(TimeInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    *lpTimeAdjustment = (DWORD)TimeInfo.TimeAdjustment;
    *lpTimeIncrement = (DWORD)TimeInfo.TimeIncrement;
    *lpTimeAdjustmentDisabled = (BOOL)TimeInfo.Enable;

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetSystemTimeAdjustment(IN DWORD dwTimeAdjustment,
                        IN BOOL bTimeAdjustmentDisabled)
{
    NTSTATUS Status;
    SYSTEM_SET_TIME_ADJUST_INFORMATION TimeInfo;

    TimeInfo.TimeAdjustment = (ULONG)dwTimeAdjustment;
    TimeInfo.Enable = (BOOLEAN)bTimeAdjustmentDisabled;

    Status = NtSetSystemInformation(SystemTimeAdjustmentInformation,
                                    &TimeInfo,
                                    sizeof(TimeInfo));
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
GetSystemTimes(OUT LPFILETIME lpIdleTime OPTIONAL,
               OUT LPFILETIME lpKernelTime OPTIONAL,
               OUT LPFILETIME lpUserTime OPTIONAL)
{
    PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION ProcPerfInfo;
    LARGE_INTEGER TotalUserTime, TotalKernTime, TotalIdleTime;
    ULONG BufferSize, ReturnLength;
    CCHAR i;
    NTSTATUS Status;

    TotalUserTime.QuadPart = TotalKernTime.QuadPart = TotalIdleTime.QuadPart = 0;

    BufferSize = sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) *
                 BaseStaticServerData->SysInfo.NumberOfProcessors;

    ProcPerfInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (!ProcPerfInfo)
    {
        BaseSetLastNTError(STATUS_NO_MEMORY);
        return FALSE;
    }

    Status = NtQuerySystemInformation(SystemProcessorPerformanceInformation,
                                      ProcPerfInfo,
                                      BufferSize,
                                      &ReturnLength);
    if ((NT_SUCCESS(Status)) && (ReturnLength == BufferSize))
    {
        if (lpIdleTime)
        {
            for (i = 0; i < BaseStaticServerData->SysInfo.NumberOfProcessors; i++)
            {
                TotalIdleTime.QuadPart += ProcPerfInfo[(unsigned char)i].IdleTime.QuadPart;
            }

            lpIdleTime->dwLowDateTime = TotalIdleTime.LowPart;
            lpIdleTime->dwHighDateTime = TotalIdleTime.HighPart;
        }

        if (lpKernelTime)
        {
            for (i = 0; i < BaseStaticServerData->SysInfo.NumberOfProcessors; i++)
            {
                TotalKernTime.QuadPart += ProcPerfInfo[(unsigned char)i].KernelTime.QuadPart;
            }

            lpKernelTime->dwLowDateTime = TotalKernTime.LowPart;
            lpKernelTime->dwHighDateTime = TotalKernTime.HighPart;
        }

        if (lpUserTime)
        {
            for (i = 0; i < BaseStaticServerData->SysInfo.NumberOfProcessors; i++)
            {
                TotalUserTime.QuadPart += ProcPerfInfo[(unsigned char)i].UserTime.QuadPart;
            }

            lpUserTime->dwLowDateTime = TotalUserTime.LowPart;
            lpUserTime->dwHighDateTime = TotalUserTime.HighPart;
        }
    }
    else if (NT_SUCCESS(Status))
    {
         Status = STATUS_INTERNAL_ERROR;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, ProcPerfInfo);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetClientTimeZoneInformation(IN CONST TIME_ZONE_INFORMATION *lpTimeZoneInformation)
{
    /* kernel32's STUB macro */
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    DPRINT1("%s() is UNIMPLEMENTED!\n", __FUNCTION__);
    return 0;
}

static
ULONGLONG
BasepQueryInterruptTimePrecise(BOOLEAN Unbiased)
{
    LARGE_INTEGER Counter;
    ULONGLONG Time, Baseline, Increment, Bias;
    ULONG Sequence;
    UCHAR Shift;

    for (;;)
    {
        Sequence = ReadULongAcquire((volatile ULONG *)&SharedUserData->TimeUpdateLock);
        if (Sequence & 1)
        {
            YieldProcessor();
            continue;
        }
        Time = KiReadSystemTime(&SharedUserData->InterruptTime).QuadPart;
        Baseline = ReadULong64NoFence(&SharedUserData->BaselineInterruptTimeQpc);
        Increment = ReadULong64NoFence(&SharedUserData->QpcInterruptTimeIncrement);
        Shift = ReadUCharNoFence(&SharedUserData->QpcInterruptTimeIncrementShift);
        Bias = Unbiased ? ReadULong64NoFence(&SharedUserData->InterruptTimeBias) : 0;
        RtlQueryPerformanceCounter(&Counter);
        MemoryBarrier();
        if (Sequence == ReadULongAcquire((volatile ULONG *)&SharedUserData->TimeUpdateLock)) break;
    }
    return Time +
        RtlpScaleTimeDelta(Counter.QuadPart - Baseline, Increment, Shift, NULL) - Bias;
}

VOID
WINAPI
QueryInterruptTime(_Out_ PULONGLONG lpInterruptTime)
{
    *lpInterruptTime = KiReadSystemTime(&SharedUserData->InterruptTime).QuadPart;
}

VOID
WINAPI
QueryInterruptTimePrecise(_Out_ PULONGLONG lpInterruptTimePrecise)
{
    *lpInterruptTimePrecise = BasepQueryInterruptTimePrecise(FALSE);
}

VOID
WINAPI
QueryUnbiasedInterruptTimePrecise(_Out_ PULONGLONG lpUnbiasedInterruptTimePrecise)
{
    *lpUnbiasedInterruptTimePrecise = BasepQueryInterruptTimePrecise(TRUE);
}

/* EOF */
