/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VerifyVersionInfoW hierarchical version comparison
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>

START_TEST(VerifyVersionInfo)
{
    OSVERSIONINFOEXW CurrentVersion;
    OSVERSIONINFOEXW TestVersion;
    DWORDLONG CondMask;
    BOOL Result;

    CurrentVersion.dwOSVersionInfoSize = sizeof(CurrentVersion);
    GetVersionExW((LPOSVERSIONINFOW)&CurrentVersion);

    ok(CurrentVersion.dwMajorVersion > 0, "GetVersionExW failed\n");

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == TRUE, "Current major == current major: got %d (expected TRUE)\n", Result);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwMajorVersion--;
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == TRUE, "Current major >= (current-1) major: got %d (expected TRUE)\n", Result);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwMajorVersion++;
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == FALSE, "Current major >= (current+1) major: got %d (expected FALSE)\n", Result);
    ok(GetLastError() == ERROR_OLD_WIN_VERSION,
       "Current major >= (current+1) major: gle %lu (expected %u)\n",
       GetLastError(), ERROR_OLD_WIN_VERSION);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwMinorVersion = (CurrentVersion.dwMinorVersion > 0) ? CurrentVersion.dwMinorVersion - 1 : 0;
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
    CondMask = VerSetConditionMask(CondMask, VER_MINORVERSION, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION | VER_MINORVERSION, CondMask);
    ok(Result == TRUE, "Hierarchical: major equal, minor >=: got %d (expected TRUE)\n", Result);

    /*
     * Windows reference table (Pam, Win11 26100, vvi_table2.txt). VerSetConditionMask
     * sets bit 63; without it a listed field with a zero condition must fail with
     * ERROR_OLD_WIN_VERSION instead of passing.
     */
    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwBuildNumber = CurrentVersion.dwBuildNumber > 0 ? CurrentVersion.dwBuildNumber - 1 : 0;
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion,
                                VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
                                CondMask);
    ok(Result == TRUE, "ODT row: build >= current-1, unconditioned major/minor: got %d (expected TRUE)\n", Result);
    ok(GetLastError() == ERROR_SUCCESS, "ODT row: gle %lu (expected 0)\n", GetLastError());

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwBuildNumber = CurrentVersion.dwBuildNumber > 0 ? CurrentVersion.dwBuildNumber - 1 : 0;
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    CondMask &= ~0x8000000000000000ULL;
    Result = VerifyVersionInfoW(&TestVersion,
                                VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
                                CondMask);
    ok(Result == FALSE, "ODT row without bit 63: got %d (expected FALSE)\n", Result);
    ok(GetLastError() == ERROR_OLD_WIN_VERSION,
       "ODT row without bit 63: gle %lu (expected %u)\n",
       GetLastError(), ERROR_OLD_WIN_VERSION);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MINORVERSION, CondMask);
    ok(Result == TRUE, "Table row: only MINOR listed, build EQ in mask: got %d (expected TRUE)\n", Result);
    ok(GetLastError() == ERROR_SUCCESS, "Table row: gle %lu (expected 0)\n", GetLastError());

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwBuildNumber = 0;
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MINORVERSION, CondMask);
    ok(Result == TRUE, "Table row: build >= 0, only MINOR listed: got %d (expected TRUE)\n", Result);
    ok(GetLastError() == ERROR_SUCCESS, "Table row: gle %lu (expected 0)\n", GetLastError());

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwBuildNumber = CurrentVersion.dwBuildNumber + 1;
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion,
                                VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
                                CondMask);
    ok(Result == FALSE, "Table row: build >= current+1: got %d (expected FALSE)\n", Result);
    ok(GetLastError() == ERROR_OLD_WIN_VERSION,
       "Table row: build >= current+1: gle %lu (expected %u)\n",
       GetLastError(), ERROR_OLD_WIN_VERSION);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    SetLastError(ERROR_SUCCESS);
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == TRUE, "Table row: major >= current: got %d (expected TRUE)\n", Result);
    ok(GetLastError() == ERROR_SUCCESS, "Table row: gle %lu (expected 0)\n", GetLastError());
}
