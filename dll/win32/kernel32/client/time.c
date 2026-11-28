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

/* EOF */
