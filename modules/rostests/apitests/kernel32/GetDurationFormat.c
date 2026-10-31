/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     GetDurationFormat and GetDurationFormatEx, with the examples of the published table
 */

#include "precomp.h"

typedef int (WINAPI *PFN_EX)(LPCWSTR, DWORD, const SYSTEMTIME *, ULONGLONG, LPCWSTR, LPWSTR, int);
typedef int (WINAPI *PFN_LCID)(LCID, DWORD, const SYSTEMTIME *, ULONGLONG, LPCWSTR, LPWSTR, int);

static PFN_EX pEx;
static PFN_LCID pLcid;

static
void
CheckSt(const SYSTEMTIME *Duration, ULONGLONG Ticks, PCWSTR Format, PCWSTR Expected)
{
    WCHAR Buffer[128];
    int Result, Required;

    Buffer[0] = L'?';
    Result = pEx(LOCALE_NAME_INVARIANT, 0, Duration, Ticks, Format, Buffer, ARRAYSIZE(Buffer));
    ok(Result == (int)wcslen(Expected) + 1, "'%ls': returned %d, expected %d\n", Format, Result, (int)wcslen(Expected) + 1);
    ok(!wcscmp(Buffer, Expected), "'%ls': got '%ls', expected '%ls'\n", Format, Buffer, Expected);

    /* A zero size asks for the length, terminating null included */
    Required = pEx(LOCALE_NAME_INVARIANT, 0, Duration, Ticks, Format, NULL, 0);
    ok(Required == (int)wcslen(Expected) + 1, "'%ls': required size %d, expected %d\n", Format, Required, (int)wcslen(Expected) + 1);
}

static
void
Test_Examples(void)
{
    /* 14 days, 2 hours, 45 minutes, 12 seconds, 247 milliseconds; year, month and day of week are ignored */
    SYSTEMTIME Long = { 1999, 5, 3, 14, 2, 45, 12, 247 };
    /* 345 seconds */
    SYSTEMTIME Short = { 0, 0, 0, 0, 0, 5, 45, 0 };

    CheckSt(&Long, 0, L"d:hh:mm:ss", L"14:02:45:12");
    CheckSt(&Long, 0, L"hh:mm:ss:ff", L"338:45:12:24");
    CheckSt(&Long, 0, L"hh:mm:ss:fff", L"338:45:12:247");
    CheckSt(&Long, 0, L"h' h 'mm' m 'ss' s'", L"338 h 45 m 12 s");

    CheckSt(&Short, 0, L"hh:mm:ss", L"00:05:45");
    CheckSt(&Short, 0, L"h:mm:ss", L"0:05:45");
    CheckSt(&Short, 0, L"mm:ss", L"05:45");
    CheckSt(&Short, 0, L"m:ss", L"5:45");
    CheckSt(&Short, 0, L"mm' m 'ss' s'", L"05 m 45 s");
    CheckSt(&Short, 0, L"ss", L"345");
    CheckSt(&Short, 0, L"ss' seconds'", L"345 seconds");

    /* 5.1234567 seconds; digits are truncated, not rounded */
    CheckSt(NULL, 51234567, L"s.fff", L"5.123");
    CheckSt(NULL, 51234567, L"s.ffffff", L"5.123456");
    CheckSt(NULL, 51234567, L"s.fffffffff", L"5.123456700");
    CheckSt(NULL, 51234567, L"fff 'ms'", L"5123 ms");
    CheckSt(NULL, 51234567, L"ffffff 'microseconds'", L"5123456 microseconds");
    CheckSt(NULL, 51234567, L"fffffffff 'ns'", L"5123456700 ns");
}

static
void
Test_Rules(void)
{
    SYSTEMTIME Both = { 0, 0, 0, 0, 0, 0, 1, 0 };
    WCHAR Buffer[64];
    int Result;

    /* A structure together with a tick count is refused (measured on Windows 11: result 0, error 87) */
    SetLastError(0xDEADBEEF);
    Result = pEx(LOCALE_NAME_INVARIANT, 0, &Both, 600000000ULL, L"ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 0, "Structure and ticks: %d '%ls'\n", Result, Buffer);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "Structure and ticks: error %lu\n", GetLastError());

    /* "ss" has a leading zero, "s" does not (measured on Windows 11) */
    Result = pEx(LOCALE_NAME_INVARIANT, 0, &Both, 0, L"ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 3 && !wcscmp(Buffer, L"01"), "Structure, ss: %d '%ls'\n", Result, Buffer);
    Result = pEx(LOCALE_NAME_INVARIANT, 0, &Both, 0, L"s", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 2 && !wcscmp(Buffer, L"1"), "Structure, s: %d '%ls'\n", Result, Buffer);

    /* No structure and no ticks is a zero duration */
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 0, L"hh:mm:ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 9 && !wcscmp(Buffer, L"00:00:00"), "Zero duration: %d '%ls'\n", Result, Buffer);

    /* H is an alias of h */
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 36000000000ULL, L"HH", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 3 && !wcscmp(Buffer, L"01"), "HH: %d '%ls'\n", Result, Buffer);

    /* Days are not bounded */
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 1000ULL * 864000000000ULL, L"d", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 5 && !wcscmp(Buffer, L"1000"), "Days: %d '%ls'\n", Result, Buffer);

    /* The first field is normalized, so an hour and five minutes are 65 minutes */
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 36000000000ULL + 5 * 600000000ULL, L"m", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 3 && !wcscmp(Buffer, L"65"), "Minutes: %d '%ls'\n", Result, Buffer);
}

static
void
Test_Errors(void)
{
    WCHAR Buffer[4];
    int Result;

    /* The buffer is too small */
    SetLastError(0xDEADBEEF);
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 0, L"hh:mm:ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 0, "A short buffer returned %d\n", Result);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "error %lu\n", GetLastError());

    /* A buffer length without a buffer: the documentation lists a NULL buffer under the size error */
    SetLastError(0xDEADBEEF);
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 0, L"ss", NULL, 10);
    ok(Result == 0, "A missing buffer returned %d\n", Result);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "error %lu\n", GetLastError());

    /* Flags must be 0 when a picture is given */
    SetLastError(0xDEADBEEF);
    Result = pEx(LOCALE_NAME_INVARIANT, 1, NULL, 0, L"ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 0, "A flag with a picture returned %d\n", Result);

    /* A locale that does not exist */
    SetLastError(0xDEADBEEF);
    Result = pEx(L"xx-NOTALOCALE", 0, NULL, 0, L"ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 0, "An invalid locale returned %d\n", Result);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    /* A negative size */
    SetLastError(0xDEADBEEF);
    Result = pEx(LOCALE_NAME_INVARIANT, 0, NULL, 0, L"ss", Buffer, -1);
    ok(Result == 0, "A negative size returned %d\n", Result);
}

static
void
Test_Lcid(void)
{
    SYSTEMTIME Short = { 0, 0, 0, 0, 0, 5, 45, 0 };
    WCHAR Buffer[32];
    int Result;

    Result = pLcid(MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, &Short, 0, L"m:ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 5 && !wcscmp(Buffer, L"5:45"), "GetDurationFormat: %d '%ls'\n", Result, Buffer);

    Result = pLcid(LOCALE_USER_DEFAULT, 0, &Short, 0, L"m:ss", Buffer, ARRAYSIZE(Buffer));
    ok(Result == 5 && !wcscmp(Buffer, L"5:45"), "Default locale: %d '%ls'\n", Result, Buffer);
}

START_TEST(GetDurationFormat)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    pEx = (PFN_EX)GetProcAddress(Kernel32, "GetDurationFormatEx");
    pLcid = (PFN_LCID)GetProcAddress(Kernel32, "GetDurationFormat");
    ok(pEx != NULL, "GetDurationFormatEx is not exported\n");
    ok(pLcid != NULL, "GetDurationFormat is not exported\n");
    if (!pEx || !pLcid)
        return;

    Test_Examples();
    Test_Rules();
    Test_Errors();
    Test_Lcid();
}
