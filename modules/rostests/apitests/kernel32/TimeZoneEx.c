/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SystemTimeToTzSpecificLocalTimeEx and TzSpecificLocalTimeToSystemTimeEx
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_CONVERT)(const DYNAMIC_TIME_ZONE_INFORMATION *, const SYSTEMTIME *, LPSYSTEMTIME);
typedef BOOL (WINAPI *PFN_FORYEAR)(USHORT, PDYNAMIC_TIME_ZONE_INFORMATION, LPTIME_ZONE_INFORMATION);

static PFN_CONVERT pToLocal;
static PFN_CONVERT pToUniversal;
static PFN_FORYEAR pForYear;

static
SYSTEMTIME
Time(WORD Year, WORD Month, WORD Day, WORD Hour, WORD Minute, WORD Second)
{
    SYSTEMTIME St = { Year, Month, 0, Day, Hour, Minute, Second, 0 };

    return St;
}

static
BOOL
LoadZone(PCWSTR Key, DYNAMIC_TIME_ZONE_INFORMATION *Zone)
{
    TIME_ZONE_INFORMATION Tzi;

    ZeroMemory(Zone, sizeof(*Zone));
    StringCchCopyW(Zone->TimeZoneKeyName, ARRAYSIZE(Zone->TimeZoneKeyName), Key);
    return pForYear(2024, Zone, &Tzi);
}

static
void
ExpectLocal(const DYNAMIC_TIME_ZONE_INFORMATION *Zone, SYSTEMTIME Utc, WORD Day, WORD Hour, WORD Minute, PCSTR What)
{
    SYSTEMTIME Local;
    BOOL Result = pToLocal(Zone, &Utc, &Local);

    ok(Result, "%s: failed with %lu\n", What, GetLastError());
    if (Result)
        ok(Local.wDay == Day && Local.wHour == Hour && Local.wMinute == Minute,
           "%s: got day %u %02u:%02u, expected day %u %02u:%02u\n", What,
           Local.wDay, Local.wHour, Local.wMinute, Day, Hour, Minute);
}

static
void
ExpectUniversal(const DYNAMIC_TIME_ZONE_INFORMATION *Zone, SYSTEMTIME Local, WORD Day, WORD Hour, PCSTR What)
{
    SYSTEMTIME Utc;
    BOOL Result = pToUniversal(Zone, &Local, &Utc);

    ok(Result, "%s: failed with %lu\n", What, GetLastError());
    if (Result)
        ok(Utc.wDay == Day && Utc.wHour == Hour,
           "%s: got day %u %02u:00, expected day %u %02u:00\n", What, Utc.wDay, Utc.wHour, Day, Hour);
}

static
void
Test_Invalid(void)
{
    SYSTEMTIME In = Time(2024, 6, 1, 12, 0, 0), Out;
    SYSTEMTIME Bad = Time(2024, 13, 1, 12, 0, 0);
    DYNAMIC_TIME_ZONE_INFORMATION Unknown;

    ok(!pToLocal(NULL, NULL, &Out), "NULL time converted\n");
    ok(!pToLocal(NULL, &In, NULL), "NULL result accepted\n");
    ok(!pToLocal(NULL, &Bad, &Out), "Month 13 converted\n");
    ok(!pToUniversal(NULL, NULL, &Out), "NULL time converted back\n");
    ok(!pToUniversal(NULL, &In, NULL), "NULL result accepted back\n");
    ok(!pToUniversal(NULL, &Bad, &Out), "Month 13 converted back\n");

    ZeroMemory(&Unknown, sizeof(Unknown));
    StringCchCopyW(Unknown.TimeZoneKeyName, ARRAYSIZE(Unknown.TimeZoneKeyName), L"No Such Time Zone Exists");
    ok(!pToLocal(&Unknown, &In, &Out), "A zone that does not exist converted\n");
    ok(!pToUniversal(&Unknown, &In, &Out), "A zone that does not exist converted back\n");
}

static
void
Test_ActiveZone(void)
{
    SYSTEMTIME In, Ex, Plain;
    TIME_ZONE_INFORMATION Tzi;
    int Month;

    /* No zone means the active one, resolved for the year of the time */
    for (Month = 1; Month <= 12; Month += 6)
    {
        In = Time(2024, (WORD)Month, 15, 12, 0, 0);
        ok(pForYear(2024, NULL, &Tzi), "GetTimeZoneInformationForYear failed: %lu\n", GetLastError());
        ok(pToLocal(NULL, &In, &Ex), "Conversion failed: %lu\n", GetLastError());
        ok(SystemTimeToTzSpecificLocalTime(&Tzi, &In, &Plain), "Plain conversion failed\n");
        ok(RtlEqualMemory(&Ex, &Plain, sizeof(Ex)), "Month %d: Ex gave %02u:%02u, plain gave %02u:%02u\n",
           Month, Ex.wHour, Ex.wMinute, Plain.wHour, Plain.wMinute);
    }
}

/*
 * United States rules since 2007: daylight time starts the second Sunday of March
 * at 02:00 and ends the first Sunday of November at 02:00
 */
static
void
Test_PacificEdges(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION Zone;

    if (!LoadZone(L"Pacific Standard Time", &Zone))
    {
        skip("Pacific Standard Time is not in the registry\n");
        return;
    }

    /* 2024-03-10: 02:00 standard time does not exist, the clocks jump to 03:00 */
    ExpectLocal(&Zone, Time(2024, 3, 10, 9, 59, 59), 10, 1, 59, "last second of standard time");
    ExpectLocal(&Zone, Time(2024, 3, 10, 10, 0, 0), 10, 3, 0, "first second of daylight time");
    /* 2024-11-03: 02:00 daylight time falls back to 01:00 */
    ExpectLocal(&Zone, Time(2024, 11, 3, 8, 59, 59), 3, 1, 59, "last second of daylight time");
    ExpectLocal(&Zone, Time(2024, 11, 3, 9, 0, 0), 3, 1, 0, "first second back in standard time");
    /* Away from the edges */
    ExpectLocal(&Zone, Time(2024, 1, 15, 12, 0, 0), 15, 4, 0, "winter");
    ExpectLocal(&Zone, Time(2024, 7, 15, 12, 0, 0), 15, 5, 0, "summer");
    /* UTC has reached the new year, Pacific time has not */
    ExpectLocal(&Zone, Time(2025, 1, 1, 3, 0, 0), 31, 19, 0, "new year");

    ExpectUniversal(&Zone, Time(2024, 7, 15, 5, 0, 0), 15, 12, "summer back");
    ExpectUniversal(&Zone, Time(2024, 1, 15, 4, 0, 0), 15, 12, "winter back");
}

/* A zone without daylight saving time shows the same offset all year */
static
void
Test_NoDaylightZone(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION Zone;

    if (!LoadZone(L"US Mountain Standard Time", &Zone))
    {
        skip("US Mountain Standard Time is not in the registry\n");
        return;
    }

    ExpectLocal(&Zone, Time(2024, 1, 15, 12, 0, 0), 15, 5, 0, "winter");
    ExpectLocal(&Zone, Time(2024, 7, 15, 12, 0, 0), 15, 5, 0, "summer");
    ExpectLocal(&Zone, Time(2024, 3, 10, 10, 0, 0), 10, 3, 0, "the day the Pacific zone changes");
}

START_TEST(TimeZoneEx)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pToLocal = (PFN_CONVERT)GetProcAddress(Kernel32, "SystemTimeToTzSpecificLocalTimeEx");
    pToUniversal = (PFN_CONVERT)GetProcAddress(Kernel32, "TzSpecificLocalTimeToSystemTimeEx");
    pForYear = (PFN_FORYEAR)GetProcAddress(Kernel32, "GetTimeZoneInformationForYear");
    ok(pToLocal != NULL, "SystemTimeToTzSpecificLocalTimeEx is not exported\n");
    ok(pToUniversal != NULL, "TzSpecificLocalTimeToSystemTimeEx is not exported\n");
    if (!pToLocal || !pToUniversal || !pForYear)
        return;

    Test_Invalid();
    Test_ActiveZone();
    Test_PacificEdges();
    Test_NoDaylightZone();
}
