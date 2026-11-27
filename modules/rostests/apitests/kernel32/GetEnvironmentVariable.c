/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for GetEnvironmentVariable(A/W)
 * COPYRIGHT:   Copyright 2026 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static void Test_GetEnvironmentVariableA(void)
{
    CHAR Buffer[MAX_PATH];
    ULONG ActualLength, Length;

    /* Get the COMSPEC variable */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableA("COMSPEC", Buffer, ARRAYSIZE(Buffer));
    Buffer[MAX_PATH - 1] = '\0';
    ActualLength = (ULONG)strlen(Buffer);
    ok(ActualLength < ARRAYSIZE(Buffer) - 1, "ActualLength == ARRAYSIZE(Buffer) - 1. Not null-terminated?\n");
    ok_eq_ulong(Length, ActualLength);
    ok_eq_char(Buffer[ActualLength + 1], '\xAB');

    /* Get the length only (return value is including NULL char) */
    Length = GetEnvironmentVariableA("COMSPEC", NULL, 0);
    ok_eq_ulong(Length, ActualLength + 1);

    /* Test a buffer size that is too small */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableA("COMSPEC", Buffer, 5);
    ok_eq_ulong(Length, ActualLength + 1);
    ok_eq_char(Buffer[0], '\xAB');

    /* Test a buffer size that doesn't fit the terminating NULL char */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableA("COMSPEC", Buffer, ActualLength);
    ok_eq_ulong(Length, ActualLength + 1);
    ok_eq_char(Buffer[0], '\xAB');

    /* Test a buffer size that is just large enough */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableA("COMSPEC", Buffer, ActualLength + 1);
    ok_eq_ulong(Length, ActualLength);
    ok_eq_char(Buffer[ActualLength], '\0');
    ok_eq_char(Buffer[ActualLength + 1], '\xAB');

    /* Test non-existant variable name */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableA("ThisVariableDoesNotExit", Buffer, ARRAYSIZE(Buffer));
    ok_eq_ulong(Length, 0);
    ok_eq_wchar(Buffer[0], '\xAB');

    /* Test NULL variable name */
    Length = GetEnvironmentVariableA(NULL, Buffer, ARRAYSIZE(Buffer));
    ok_eq_ulong(Length, 0);

    /* Test NULL buffer with non-null size */
    StartSeh()
        Length = GetEnvironmentVariableA("COMSPEC", NULL, ARRAYSIZE(Buffer));
    EndSeh(STATUS_ACCESS_VIOLATION);
}

static void Test_GetEnvironmentVariableW(void)
{
    WCHAR Buffer[MAX_PATH];
    ULONG ActualLength, Length;

    /* Get the COMSPEC variable */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableW(L"COMSPEC", Buffer, ARRAYSIZE(Buffer));
    Buffer[MAX_PATH - 1] = L'\0';
    ActualLength = (ULONG)wcslen(Buffer);
    ok(ActualLength < ARRAYSIZE(Buffer) - 1, "ActualLength == ARRAYSIZE(Buffer) - 1. Not null-terminated?\n");
    ok_eq_ulong(Length, ActualLength);
    ok_eq_wchar(Buffer[ActualLength + 1], 0xABAB);

    /* Get the length only (return value is including NULL char) */
    Length = GetEnvironmentVariableW(L"COMSPEC", NULL, 0);
    ok_eq_ulong(Length, ActualLength + 1);

    /* Test a buffer size that is too small */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableW(L"COMSPEC", Buffer, 2);
    ok_eq_ulong(Length, ActualLength + 1);
    ok(Buffer[0] == UNICODE_NULL || broken(/* Windows 2003 */ Buffer[0] == 0xABAB), "Buffer[0] = 0x%x\n", Buffer[0]);
    ok_eq_wchar(Buffer[1], 0xABAB);

    /* Test a buffer size that doesn't fit the terminating NULL char */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableW(L"COMSPEC", Buffer, ActualLength);
    ok_eq_ulong(Length, ActualLength + 1);
    ok(Buffer[0] == UNICODE_NULL || broken(/* Windows 2003 */ Buffer[0] == 0xABAB), "Buffer[0] = 0x%x\n", Buffer[0]);
    ok_eq_wchar(Buffer[1], 0xABAB);

    /* Test a buffer size that is just large enough */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableW(L"COMSPEC", Buffer, ActualLength + 1);
    ok_eq_ulong(Length, ActualLength);
    ok_eq_wchar(Buffer[ActualLength], L'\0');
    ok_eq_wchar(Buffer[ActualLength + 1], 0xABAB);

    /* Test non-existant variable name */
    memset(Buffer, 0xAB, sizeof(Buffer));
    Length = GetEnvironmentVariableW(L"ThisVariableDoesNotExit", Buffer, ARRAYSIZE(Buffer));
    ok_eq_ulong(Length, 0);
    ok_eq_wchar(Buffer[0], 0xABAB);

    /* Test NULL variable name */
    StartSeh()
        Length = GetEnvironmentVariableW(NULL, Buffer, ARRAYSIZE(Buffer));
    EndSeh(STATUS_SUCCESS);
    ok_eq_ulong(Length, 0);

    /* Test NULL buffer with non-null size */
    StartSeh()
        Length = GetEnvironmentVariableW(L"COMSPEC", NULL, ARRAYSIZE(Buffer));
    EndSeh(STATUS_SUCCESS);
    ok((Length == ActualLength + 1) || broken(/* Windows 2003 */ Length == 0), "Length == %lu, ActualLength == %lu\n", Length, ActualLength);
}

typedef NTSTATUS (NTAPI *FN_RtlSetEnvironmentStrings)(PCWCHAR, SIZE_T);

static BOOLEAN SetLongVariable(FN_RtlSetEnvironmentStrings pRtlSetEnvironmentStrings, PWSTR Block, ULONG Chars)
{
    ULONG Index;

    Block[0] = L'L';
    Block[1] = L'=';
    for (Index = 0; Index < Chars; Index++)
        Block[2 + Index] = L'y';
    Block[2 + Chars] = Block[3 + Chars] = UNICODE_NULL;
    return NT_SUCCESS(pRtlSetEnvironmentStrings(Block, (Chars + 4) * sizeof(WCHAR)));
}

/* Rows measured on Windows 11 26100: the A variant stops at UNICODE_STRING_MAX_CHARS, the W variant does not */
static void Test_LongValues(void)
{
    FN_RtlSetEnvironmentStrings pRtlSetEnvironmentStrings;
    PWSTR Saved = NULL, Current, Block, BufferW;
    PCHAR BufferA;
    SIZE_T SavedSize, Index;

    pRtlSetEnvironmentStrings = (FN_RtlSetEnvironmentStrings)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                            "RtlSetEnvironmentStrings");
    if (!pRtlSetEnvironmentStrings)
    {
        skip("RtlSetEnvironmentStrings is not exported\n");
        return;
    }

    Current = GetEnvironmentStringsW();
    for (Index = 0; Current[Index] || Current[Index + 1]; Index++);
    SavedSize = (Index + 2) * sizeof(WCHAR);
    Saved = HeapAlloc(GetProcessHeap(), 0, SavedSize);
    Block = HeapAlloc(GetProcessHeap(), 0, 40004 * sizeof(WCHAR));
    BufferA = HeapAlloc(GetProcessHeap(), 0, 40008);
    BufferW = HeapAlloc(GetProcessHeap(), 0, 40008 * sizeof(WCHAR));
    if (!Saved || !Block || !BufferA || !BufferW)
    {
        skip("no memory\n");
        FreeEnvironmentStringsW(Current);
        goto Cleanup;
    }
    RtlCopyMemory(Saved, Current, SavedSize);
    FreeEnvironmentStringsW(Current);

    ok(SetLongVariable(pRtlSetEnvironmentStrings, Block, 32766), "set 32766 failed\n");
    ok_eq_ulong(GetEnvironmentVariableA("L", NULL, 0), 32767);
    memset(BufferA, 0xAB, 40008);
    ok_eq_ulong(GetEnvironmentVariableA("L", BufferA, 32766), 32767);
    ok_eq_char(BufferA[0], '\xAB');
    ok_eq_ulong(GetEnvironmentVariableA("L", BufferA, 32767), 32766);
    ok_eq_char(BufferA[32765], 'y');
    ok_eq_char(BufferA[32766], '\0');
    ok_eq_ulong(GetEnvironmentVariableW(L"L", NULL, 0), 32767);
    ok_eq_ulong(GetEnvironmentVariableW(L"L", BufferW, 32767), 32766);

    ok(SetLongVariable(pRtlSetEnvironmentStrings, Block, 32767), "set 32767 failed\n");
    SetLastError(0xdeadbeef);
    ok_eq_ulong(GetEnvironmentVariableA("L", NULL, 0), 0);
    ok_eq_ulong(GetLastError(), ERROR_NOT_ENOUGH_MEMORY);
    memset(BufferA, 0xAB, 40008);
    SetLastError(0xdeadbeef);
    ok_eq_ulong(GetEnvironmentVariableA("L", BufferA, 32768), 0);
    ok_eq_ulong(GetLastError(), ERROR_NOT_ENOUGH_MEMORY);
    ok_eq_char(BufferA[0], '\xAB');
    ok_eq_ulong(GetEnvironmentVariableW(L"L", NULL, 0), 32768);
    ok_eq_ulong(GetEnvironmentVariableW(L"L", BufferW, 32768), 32767);

    ok(SetLongVariable(pRtlSetEnvironmentStrings, Block, 40000), "set 40000 failed\n");
    SetLastError(0xdeadbeef);
    ok_eq_ulong(GetEnvironmentVariableA("L", BufferA, 40001), 0);
    ok_eq_ulong(GetLastError(), ERROR_NOT_ENOUGH_MEMORY);
    ok_eq_ulong(GetEnvironmentVariableW(L"L", NULL, 0), 40001);
    ok_eq_ulong(GetEnvironmentVariableW(L"L", BufferW, 40001), 40000);
    ok_eq_wchar(BufferW[39999], L'y');
    ok_eq_wchar(BufferW[40000], UNICODE_NULL);

    pRtlSetEnvironmentStrings(Saved, SavedSize);

Cleanup:
    if (Saved) HeapFree(GetProcessHeap(), 0, Saved);
    if (Block) HeapFree(GetProcessHeap(), 0, Block);
    if (BufferA) HeapFree(GetProcessHeap(), 0, BufferA);
    if (BufferW) HeapFree(GetProcessHeap(), 0, BufferW);
}

static PWSTR MakeString(WCHAR Fill, SIZE_T Length)
{
    PWSTR String;
    SIZE_T Index;

    String = HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
    if (String)
    {
        for (Index = 0; Index < Length; Index++)
            String[Index] = Fill;
        String[Length] = UNICODE_NULL;
    }
    return String;
}

/* Rows measured on Windows 11 26100: SetEnvironmentVariableW has no UNICODE_STRING limit */
static void Test_SetLongValues(void)
{
    PWSTR Long, Name32767, Name40000;

    Long = MakeString(L'y', 70000);
    Name32767 = MakeString(L'm', 32767);
    Name40000 = MakeString(L'n', 40000);
    if (!Long || !Name32767 || !Name40000)
    {
        skip("no memory\n");
        goto Cleanup;
    }

    ok(SetEnvironmentVariableW(L"PRC", Long), "70000 chars: error %lu\n", GetLastError());
    ok_eq_ulong(GetEnvironmentVariableW(L"PRC", NULL, 0), 70001);
    Long[32767] = UNICODE_NULL;
    ok(SetEnvironmentVariableW(L"PRC", Long), "32767 chars: error %lu\n", GetLastError());
    ok_eq_ulong(GetEnvironmentVariableW(L"PRC", NULL, 0), 32768);
    ok(SetEnvironmentVariableW(L"PRC", L""), "empty value: error %lu\n", GetLastError());
    ok_eq_ulong(GetEnvironmentVariableW(L"PRC", NULL, 0), 1);
    ok(SetEnvironmentVariableW(L"PRC", NULL), "delete: error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok_eq_ulong(GetEnvironmentVariableW(L"PRC", NULL, 0), 0);
    ok_eq_ulong(GetLastError(), ERROR_ENVVAR_NOT_FOUND);

    ok(SetEnvironmentVariableW(Name32767, L"v"), "32767-char name: error %lu\n", GetLastError());
    ok_eq_ulong(GetEnvironmentVariableW(Name32767, NULL, 0), 2);
    ok(SetEnvironmentVariableW(Name40000, L"v"), "40000-char name: error %lu\n", GetLastError());
    ok_eq_ulong(GetEnvironmentVariableW(Name40000, NULL, 0), 2);
    SetEnvironmentVariableW(Name32767, NULL);
    SetEnvironmentVariableW(Name40000, NULL);

    SetLastError(0xdeadbeef);
    ok(!SetEnvironmentVariableW(L"", L"v"), "empty name accepted\n");
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);
    SetLastError(0xdeadbeef);
    ok(!SetEnvironmentVariableW(L"A=B", L"v"), "name with '=' accepted\n");
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);

Cleanup:
    if (Long) HeapFree(GetProcessHeap(), 0, Long);
    if (Name32767) HeapFree(GetProcessHeap(), 0, Name32767);
    if (Name40000) HeapFree(GetProcessHeap(), 0, Name40000);
}

START_TEST(GetEnvironmentVariable)
{
    Test_GetEnvironmentVariableA();
    Test_GetEnvironmentVariableW();
    Test_LongValues();
    Test_SetLongValues();
}
