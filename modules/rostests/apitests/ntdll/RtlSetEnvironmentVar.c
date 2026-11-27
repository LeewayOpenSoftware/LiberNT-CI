/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for RtlSetEnvironmentVar, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlSetEnvironmentVar)(PWSTR *, PCWSTR, SIZE_T, PCWSTR, SIZE_T);
typedef NTSTATUS (NTAPI *FN_RtlQueryEnvironmentVariable)(PVOID, PWSTR, SIZE_T, PWSTR, SIZE_T, PSIZE_T);

static FN_RtlSetEnvironmentVar pRtlSetEnvironmentVar;
static FN_RtlQueryEnvironmentVariable pRtlQueryEnvironmentVariable;

static
SIZE_T
QueryLength(PWSTR Environment, PCWSTR Name, SIZE_T NameLength, NTSTATUS *Status)
{
    SIZE_T ReturnLength = 0;

    *Status = pRtlQueryEnvironmentVariable(Environment, (PWSTR)Name, NameLength, NULL, 0, &ReturnLength);
    return ReturnLength;
}

static
PWSTR
MakeString(WCHAR Fill, SIZE_T Length)
{
    PWSTR String;
    SIZE_T Index;

    String = RtlAllocateHeap(RtlGetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
    if (String)
    {
        for (Index = 0; Index < Length; Index++)
            String[Index] = Fill;
        String[Length] = UNICODE_NULL;
    }
    return String;
}

START_TEST(RtlSetEnvironmentVar)
{
    PWSTR Environment = NULL, Original, Long, LongName;
    NTSTATUS Status;
    SIZE_T Length;

    pRtlSetEnvironmentVar = (FN_RtlSetEnvironmentVar)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                    "RtlSetEnvironmentVar");
    pRtlQueryEnvironmentVariable = (FN_RtlQueryEnvironmentVariable)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                                  "RtlQueryEnvironmentVariable");
    if (!pRtlSetEnvironmentVar || !pRtlQueryEnvironmentVariable)
    {
        skip("RtlSetEnvironmentVar is not exported\n");
        return;
    }

    Status = RtlCreateEnvironment(FALSE, &Environment);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Original = Environment;

    /* A private block grows into a new allocation */
    Status = pRtlSetEnvironmentVar(&Environment, L"PRIV", 4, L"abc", 3);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(Environment != Original, "block not reallocated\n");
    Length = QueryLength(Environment, L"PRIV", 4, &Status);
    ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);
    ok_size_t(Length, 4);

    /* Lengths are characters; the strings need no terminator */
    Status = pRtlSetEnvironmentVar(&Environment, L"PRBX", 3, L"vw", 1);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(QueryLength(Environment, L"PRB", 3, &Status), 2);
    QueryLength(Environment, L"PRBX", 4, &Status);
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);

    /* A zero length value is an empty value */
    Status = pRtlSetEnvironmentVar(&Environment, L"PRB", 3, L"abc", 0);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(QueryLength(Environment, L"PRB", 3, &Status), 1);
    ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);

    /* A NULL value deletes, also when the variable does not exist */
    Status = pRtlSetEnvironmentVar(&Environment, L"PRB", 3, NULL, 0);
    ok_ntstatus(Status, STATUS_SUCCESS);
    QueryLength(Environment, L"PRB", 3, &Status);
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);
    Status = pRtlSetEnvironmentVar(&Environment, L"PRB", 3, NULL, 5);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Names: not empty, '=' only as the first character */
    Status = pRtlSetEnvironmentVar(&Environment, L"", 0, L"v", 1);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    Status = pRtlSetEnvironmentVar(&Environment, L"A=B", 3, L"v", 1);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    Status = pRtlSetEnvironmentVar(&Environment, L"=X", 2, L"v", 1);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(QueryLength(Environment, L"=X", 2, &Status), 2);

    /* No UNICODE_STRING limit on values or names */
    Long = MakeString(L'y', 40000);
    LongName = MakeString(L'n', 40000);
    if (Long && LongName)
    {
        Status = pRtlSetEnvironmentVar(&Environment, L"PRA", 3, Long, 40000);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_size_t(QueryLength(Environment, L"PRA", 3, &Status), 40001);
        Status = pRtlSetEnvironmentVar(&Environment, L"PRA", 3, Long, 100);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_size_t(QueryLength(Environment, L"PRA", 3, &Status), 101);
        Status = pRtlSetEnvironmentVar(&Environment, LongName, 40000, L"v", 1);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_size_t(QueryLength(Environment, LongName, 40000, &Status), 2);
        ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);
    }
    else
    {
        skip("no memory\n");
    }
    if (Long) RtlFreeHeap(RtlGetProcessHeap(), 0, Long);
    if (LongName) RtlFreeHeap(RtlGetProcessHeap(), 0, LongName);

    RtlDestroyEnvironment(Environment);
}
