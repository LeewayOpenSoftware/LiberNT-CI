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
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_OR_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == TRUE, "Current major >= (current-1) major: got %d (expected TRUE)\n", Result);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwMajorVersion++;
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_OR_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION, CondMask);
    ok(Result == FALSE, "Current major >= (current+1) major: got %d (expected FALSE)\n", Result);

    CopyMemory(&TestVersion, &CurrentVersion, sizeof(TestVersion));
    TestVersion.dwMinorVersion = (CurrentVersion.dwMinorVersion > 0) ? CurrentVersion.dwMinorVersion - 1 : 0;
    CondMask = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
    CondMask = VerSetConditionMask(CondMask, VER_MINORVERSION, VER_GREATER_OR_EQUAL);
    Result = VerifyVersionInfoW(&TestVersion, VER_MAJORVERSION | VER_MINORVERSION, CondMask);
    ok(Result == TRUE, "Hierarchical: major equal, minor >=: got %d (expected TRUE)\n", Result);

    {
        OSVERSIONINFOEXW v;
        DWORDLONG m = 0;

        CopyMemory(&v, &CurrentVersion, sizeof(v));
        m = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_OR_EQUAL);
        Result = VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, m);
        ok(Result == TRUE,
           "Trace case (only BUILD condition, rest 0, current >= 0): got %d (expected TRUE)\n",
           Result);
    }

    {
        OSVERSIONINFOEXW v;
        DWORDLONG m = 0;

        CopyMemory(&v, &CurrentVersion, sizeof(v));
        v.dwBuildNumber++;
        m = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_OR_EQUAL);
        Result = VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, m);
        ok(Result == TRUE,
           "Trace case (only BUILD condition, target build lower): got %d (expected TRUE)\n",
           Result);
    }
}
