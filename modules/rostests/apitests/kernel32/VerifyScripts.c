/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VerifyScripts, with the cases of the published table
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_VERIFYSCRIPTS)(DWORD, LPCWSTR, int, LPCWSTR, int);

static PFN_VERIFYSCRIPTS pVerifyScripts;

static
void
Check(DWORD Flags, PCWSTR Locale, PCWSTR Test, BOOL Expected, DWORD Error, PCSTR What)
{
    BOOL Result;

    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(Flags, Locale, -1, Test, -1);
    ok(Result == Expected, "%s: returned %d, expected %d\n", What, Result, Expected);
    if (!Expected)
        ok(GetLastError() == Error, "%s: error %lu, expected %lu\n", What, GetLastError(), Error);
}

START_TEST(VerifyScripts)
{
    BOOL Result;

    pVerifyScripts = (PFN_VERIFYSCRIPTS)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VerifyScripts");
    ok(pVerifyScripts != NULL, "VerifyScripts is not exported\n");
    if (!pVerifyScripts)
        return;

    /* The table in the documentation */
    Check(0, L"Hani;Hira;Kana;", L"Hani;", TRUE, 0, "subset");
    Check(VS_ALLOW_LATIN, L"Hani;Hira;Kana;", L"Hani;", TRUE, 0, "subset with the flag");
    Check(0, L"Hani;Hira;Kana;", L"Hani;Latn;", FALSE, ERROR_SUCCESS, "Latin not allowed");
    Check(VS_ALLOW_LATIN, L"Hani;Hira;Kana;", L"Hani;Latn;", TRUE, 0, "Latin allowed");
    Check(0, L"Hani;Hira;Kana;", L"Cyrl;", FALSE, ERROR_SUCCESS, "foreign script");
    Check(VS_ALLOW_LATIN, L"Hani;Hira;Kana;", L"Cyrl;", FALSE, ERROR_SUCCESS, "foreign script with the flag");
    Check(0, L"Hani;", L"Hani;Hira;Kana;", FALSE, ERROR_SUCCESS, "locale list smaller than the test list");
    Check(0, L"Hani;Hira;Kana;", L"Cyrl", FALSE, ERROR_INVALID_PARAMETER, "no terminating semicolon");
    Check(0, L"Hani;Hira;Kana;", L"", TRUE, 0, "empty test list");

    /* A length of 0 fails for either list */
    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(0, L"Hani;", 0, L"Hani;", -1);
    ok(!Result, "A zero length locale list succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(0, L"Hani;", -1, L"Hani;", 0);
    ok(!Result, "A zero length test list succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    /* Explicit lengths: only the first test script counts */
    Result = pVerifyScripts(0, L"Hani;Hira;", 10, L"Hira;Latn;", 5);
    ok(Result, "Only the first test script counts when the length says so\n");

    /* Flags the function does not define, and null lists */
    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(0x80, L"Hani;", -1, L"Hani;", -1);
    ok(!Result, "An undefined flag was accepted\n");
    ok(GetLastError() == ERROR_INVALID_FLAGS, "error %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(0, NULL, -1, L"Hani;", -1);
    ok(!Result, "A NULL locale list succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    Result = pVerifyScripts(0, L"Hani;", -1, NULL, -1);
    ok(!Result, "A NULL test list succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
}
