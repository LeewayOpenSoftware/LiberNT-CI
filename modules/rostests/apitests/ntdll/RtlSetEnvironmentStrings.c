/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for RtlSetEnvironmentStrings, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlSetEnvironmentStrings)(PCWCHAR, SIZE_T);

static FN_RtlSetEnvironmentStrings pRtlSetEnvironmentStrings;

static
VOID
CheckVar(PCWSTR Name, PCWSTR Expected)
{
    WCHAR Value[64];
    DWORD Length;

    SetLastError(0xdeadbeef);
    Length = GetEnvironmentVariableW(Name, Value, ARRAYSIZE(Value));
    if (Expected)
    {
        ok(Length == wcslen(Expected), "%ls: length %lu\n", Name, Length);
        ok(Length && !wcscmp(Value, Expected), "%ls: '%ls', expected '%ls'\n", Name, Length ? Value : L"", Expected);
    }
    else
    {
        ok(Length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "%ls: found (%lu, %lu)\n",
           Name, Length, GetLastError());
    }
}

static
VOID
Set(PCWCHAR Block, SIZE_T Size)
{
    PRTL_USER_PROCESS_PARAMETERS Params = NtCurrentPeb()->ProcessParameters;
    SIZE_T Version = Params->EnvironmentVersion;
    NTSTATUS Status;

    Status = pRtlSetEnvironmentStrings(Block, Size);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(Params->EnvironmentSize, Size);
    ok_size_t(Params->EnvironmentVersion, Version + 1);
}

START_TEST(RtlSetEnvironmentStrings)
{
    static const WCHAR Two[] = L"A=1\0B=2\0";
    static const WCHAR Partial[] = L"LONGNAME=value\0";
    static const WCHAR NoEqual[] = L"NOEQ\0";
    PWSTR Saved, Big, Current;
    SIZE_T SavedSize, Index;

    pRtlSetEnvironmentStrings = (FN_RtlSetEnvironmentStrings)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                            "RtlSetEnvironmentStrings");
    if (!pRtlSetEnvironmentStrings)
    {
        skip("RtlSetEnvironmentStrings is not exported\n");
        return;
    }

    /* Keep a copy of the environment to put back */
    Current = GetEnvironmentStringsW();
    for (Index = 0; Current[Index] || Current[Index + 1]; Index++);
    SavedSize = (Index + 2) * sizeof(WCHAR);
    Saved = RtlAllocateHeap(RtlGetProcessHeap(), 0, SavedSize);
    if (!Saved)
    {
        skip("no memory\n");
        return;
    }
    RtlCopyMemory(Saved, Current, SavedSize);
    FreeEnvironmentStringsW(Current);

    /* The block replaces the environment */
    Set(Two, sizeof(Two));
    CheckVar(L"A", L"1");
    CheckVar(L"B", L"2");
    CheckVar(L"PATH", NULL);

    /* Size 0 copies nothing but still updates the size and version */
    Set(Two, 0);
    CheckVar(L"A", L"1");

    /* Not validated: an entry without '=' is accepted */
    Set(NoEqual, sizeof(NoEqual));
    CheckVar(L"A", NULL);

    /* A block that fits is copied in place: 4 bytes overwrite the first 2 characters */
    Set(Partial, sizeof(Partial));
    CheckVar(L"LONGNAME", L"value");
    Set(L"QQ", 2 * sizeof(WCHAR));
    CheckVar(L"QQNGNAME", L"value");

    /* A larger block than the current allocation */
    Big = RtlAllocateHeap(RtlGetProcessHeap(), 0, 40004 * sizeof(WCHAR));
    if (Big)
    {
        Big[0] = L'W';
        Big[1] = L'=';
        for (Index = 2; Index < 40002; Index++)
            Big[Index] = L'y';
        Big[40002] = Big[40003] = UNICODE_NULL;
        Set(Big, 40004 * sizeof(WCHAR));
        ok_int(GetEnvironmentVariableW(L"W", NULL, 0), 40001);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Big);
    }

    /* Put the original environment back */
    Set(Saved, SavedSize);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Saved);
}
