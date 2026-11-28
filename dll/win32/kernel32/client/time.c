/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/time.c
 * PURPOSE:         Time conversion functions
 * PROGRAMMER:      Ariadne
 *                  DOSDATE and DOSTIME structures from Onno Hovers
 * UPDATE HISTORY:
 *                  Created 19/01/99
 */

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <reactos/precisetime.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
FileTimeToDosDateTime(IN CONST FILETIME *lpFileTime,
                      OUT LPWORD lpFatDate,
                      OUT LPWORD lpFatTime)
{
    LARGE_INTEGER FileTime;
    TIME_FIELDS TimeFields;

    FileTime.HighPart = lpFileTime->dwHighDateTime;
    FileTime.LowPart = lpFileTime->dwLowDateTime;

    if (FileTime.QuadPart < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlTimeToTimeFields(&FileTime, &TimeFields);
    if ((TimeFields.Year < 1980) || (TimeFields.Year > 2107))
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    *lpFatDate = (TimeFields.Day) |
                 (TimeFields.Month << 5) |
                 ((TimeFields.Year - 1980) << 9);
    *lpFatTime = (TimeFields.Second >> 1) |
                 (TimeFields.Minute << 5) |
                 (TimeFields.Hour << 11);

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
DosDateTimeToFileTime(IN WORD wFatDate,
                      IN WORD wFatTime,
                      OUT LPFILETIME lpFileTime)
{
    TIME_FIELDS TimeFields;
    LARGE_INTEGER SystemTime;

    TimeFields.Year = (wFatDate >> 9) + 1980;
    TimeFields.Month = (wFatDate >> 5) & 0xF;
    TimeFields.Day = (wFatDate & 0x1F);
    TimeFields.Hour = (wFatTime >> 11);
    TimeFields.Minute = (wFatTime >> 5) & 0x3F;
    TimeFields.Second = (wFatTime & 0x1F) << 1;
    TimeFields.Milliseconds = 0;

    if (RtlTimeFieldsToTime(&TimeFields, &SystemTime))
    {
        lpFileTime->dwLowDateTime = SystemTime.LowPart;
        lpFileTime->dwHighDateTime = SystemTime.HighPart;
        return TRUE;
    }

    BaseSetLastNTError(STATUS_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetClientTimeZoneInformation(IN CONST TIME_ZONE_INFORMATION *lpTimeZoneInformation)
{
    STUB;
    return 0;
}

/*
 * @implemented
 */
BOOL
WINAPI
QueryUnbiasedInterruptTime(OUT PULONGLONG UnbiasedTime)
{
    LARGE_INTEGER InterruptTime;

    if (UnbiasedTime == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    InterruptTime = KiReadSystemTime(&SharedUserData->InterruptTime);
    *UnbiasedTime = (ULONGLONG)InterruptTime.QuadPart - SharedUserData->InterruptTimeBias;
    return TRUE;
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
