/*
 * PROJECT:     LiberNT KernelBase
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Time zone conversions that honour the dynamic daylight saving rules of a year
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <winternl.h>

/*
 * A dynamic time zone names a zone in the registry and may carry different
 * daylight saving rules for different years. The rules in force for the year of
 * the time being converted are resolved first, then the plain conversion runs.
 * A NULL zone means the currently active one.
 */
static
BOOL
ResolveRulesForYear(
    _In_opt_ const DYNAMIC_TIME_ZONE_INFORMATION *DynamicZone,
    _In_ USHORT Year,
    _Out_ PTIME_ZONE_INFORMATION Zone)
{
    return GetTimeZoneInformationForYear(Year, (PDYNAMIC_TIME_ZONE_INFORMATION)DynamicZone, Zone);
}

BOOL
WINAPI
SystemTimeToTzSpecificLocalTimeEx(
    _In_opt_ const DYNAMIC_TIME_ZONE_INFORMATION *TimeZoneInformation,
    _In_ const SYSTEMTIME *UniversalTime,
    _Out_ LPSYSTEMTIME LocalTime)
{
    TIME_ZONE_INFORMATION Zone;

    if (!UniversalTime || !LocalTime)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!ResolveRulesForYear(TimeZoneInformation, UniversalTime->wYear, &Zone))
        return FALSE;

    return SystemTimeToTzSpecificLocalTime(&Zone, UniversalTime, LocalTime);
}

BOOL
WINAPI
TzSpecificLocalTimeToSystemTimeEx(
    _In_opt_ const DYNAMIC_TIME_ZONE_INFORMATION *TimeZoneInformation,
    _In_ const SYSTEMTIME *LocalTime,
    _Out_ LPSYSTEMTIME UniversalTime)
{
    TIME_ZONE_INFORMATION Zone;

    if (!LocalTime || !UniversalTime)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!ResolveRulesForYear(TimeZoneInformation, LocalTime->wYear, &Zone))
        return FALSE;

    return TzSpecificLocalTimeToSystemTime(&Zone, LocalTime, UniversalTime);
}
