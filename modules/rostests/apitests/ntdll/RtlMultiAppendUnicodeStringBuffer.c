/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for RtlMultiAppendUnicodeStringBuffer, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlMultiAppendUnicodeStringBuffer)(PRTL_UNICODE_STRING_BUFFER, ULONG, PCUNICODE_STRING);

static FN_RtlMultiAppendUnicodeStringBuffer pRtlMultiAppendUnicodeStringBuffer;

static
VOID
InitStringBuffer(PRTL_UNICODE_STRING_BUFFER StringBuffer, PWCHAR Static, USHORT StaticBytes, PCWSTR Initial)
{
    USHORT Index;

    for (Index = 0; Index < StaticBytes / sizeof(WCHAR); Index++)
        Static[Index] = L'#';
    RtlZeroMemory(StringBuffer, sizeof(*StringBuffer));
    StringBuffer->ByteBuffer.Buffer = StringBuffer->ByteBuffer.StaticBuffer = (PUCHAR)Static;
    StringBuffer->ByteBuffer.Size = StringBuffer->ByteBuffer.StaticSize = StaticBytes;
    StringBuffer->String.Buffer = Static;
    StringBuffer->String.MaximumLength = StaticBytes;
    StringBuffer->String.Length = (USHORT)(wcslen(Initial) * sizeof(WCHAR));
    RtlCopyMemory(Static, Initial, StringBuffer->String.Length);
}

static
VOID
FreeStringBuffer(PRTL_UNICODE_STRING_BUFFER StringBuffer)
{
    if (StringBuffer->ByteBuffer.Buffer != StringBuffer->ByteBuffer.StaticBuffer)
        RtlFreeHeap(RtlGetProcessHeap(), 0, StringBuffer->ByteBuffer.Buffer);
}

static
VOID
Check(PCSTR Tag, USHORT StaticBytes, PCWSTR Initial, ULONG Count, PCWSTR *Parts,
      NTSTATUS ExpectedStatus, PCWSTR Expected, SIZE_T ExpectedSize, BOOLEAN ExpectStatic)
{
    RTL_UNICODE_STRING_BUFFER StringBuffer;
    UNICODE_STRING Addends[4];
    WCHAR Static[64];
    NTSTATUS Status;
    ULONG Index;

    InitStringBuffer(&StringBuffer, Static, StaticBytes, Initial);
    for (Index = 0; Index < Count; Index++)
        RtlInitUnicodeString(&Addends[Index], Parts[Index]);

    Status = pRtlMultiAppendUnicodeStringBuffer(&StringBuffer, Count, Addends);
    ok(Status == ExpectedStatus, "%s: status 0x%lx\n", Tag, Status);
    ok(StringBuffer.String.Buffer == (PWSTR)StringBuffer.ByteBuffer.Buffer, "%s: String.Buffer moved away\n", Tag);
    ok((StringBuffer.ByteBuffer.Buffer == StringBuffer.ByteBuffer.StaticBuffer) == ExpectStatic,
       "%s: static buffer use %d\n", Tag, StringBuffer.ByteBuffer.Buffer == StringBuffer.ByteBuffer.StaticBuffer);
    ok(StringBuffer.ByteBuffer.StaticBuffer == (PUCHAR)Static, "%s: StaticBuffer changed\n", Tag);
    ok_size_t(StringBuffer.ByteBuffer.StaticSize, StaticBytes);
    ok(StringBuffer.ByteBuffer.Size == ExpectedSize, "%s: Size %Iu, expected %Iu\n",
       Tag, StringBuffer.ByteBuffer.Size, ExpectedSize);
    if (NT_SUCCESS(Status))
    {
        ok_int(StringBuffer.String.Length, (int)(wcslen(Expected) * sizeof(WCHAR)));
        ok_int(StringBuffer.String.MaximumLength, (int)((wcslen(Expected) + 1) * sizeof(WCHAR)));
        ok(!wcscmp(StringBuffer.String.Buffer, Expected), "%s: '%.40ls'\n", Tag, StringBuffer.String.Buffer);
    }
    FreeStringBuffer(&StringBuffer);
}

static
VOID
TestLimits(VOID)
{
    RTL_UNICODE_STRING_BUFFER StringBuffer;
    UNICODE_STRING Addends[2];
    WCHAR Static[8];
    PWSTR Long;
    NTSTATUS Status;
    ULONG Index;

    Long = RtlAllocateHeap(RtlGetProcessHeap(), 0, 32767 * sizeof(WCHAR));
    if (!Long)
    {
        skip("no memory\n");
        return;
    }
    for (Index = 0; Index < 32767; Index++)
        Long[Index] = L'a';

    /* 32766 characters plus the NUL fit in a UNICODE_STRING */
    InitStringBuffer(&StringBuffer, Static, sizeof(Static), L"");
    Addends[0].Buffer = Long;
    Addends[0].Length = Addends[0].MaximumLength = 32766 * sizeof(WCHAR);
    Status = pRtlMultiAppendUnicodeStringBuffer(&StringBuffer, 1, Addends);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_int(StringBuffer.String.Length, 65532);
    ok_int(StringBuffer.String.MaximumLength, 65534);
    ok_size_t(StringBuffer.ByteBuffer.Size, 65534);
    FreeStringBuffer(&StringBuffer);

    /* One more character does not; nothing changes */
    InitStringBuffer(&StringBuffer, Static, sizeof(Static), L"");
    Addends[0].Length = Addends[0].MaximumLength = 32767 * sizeof(WCHAR);
    Status = pRtlMultiAppendUnicodeStringBuffer(&StringBuffer, 1, Addends);
    ok_ntstatus(Status, STATUS_NAME_TOO_LONG);
    ok_int(StringBuffer.String.Length, 0);
    ok_int(StringBuffer.String.MaximumLength, sizeof(Static));
    ok(StringBuffer.ByteBuffer.Buffer == (PUCHAR)Static, "buffer moved\n");
    ok(Static[0] == L'#', "static buffer written\n");

    /* Overflow spread over two addends */
    RtlInitUnicodeString(&Addends[1], L"bbbbbbbbbb");
    Status = pRtlMultiAppendUnicodeStringBuffer(&StringBuffer, 2, Addends);
    ok_ntstatus(Status, STATUS_NAME_TOO_LONG);
    ok_int(StringBuffer.String.Length, 0);
    FreeStringBuffer(&StringBuffer);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Long);
}

START_TEST(RtlMultiAppendUnicodeStringBuffer)
{
    static PCWSTR Three[] = { L"ab", L"cde", L"f" };
    static PCWSTR Two[] = { L"ab", L"cde" };
    static PCWSTR Grow[] = { L"abcd", L"efgh", L"ijkl" };
    static PCWSTR XyZ[] = { L"xy", L"z" };
    static PCWSTR Xyz[] = { L"xyz" };
    static PCWSTR EmptyA[] = { L"", L"a" };

    pRtlMultiAppendUnicodeStringBuffer = (FN_RtlMultiAppendUnicodeStringBuffer)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlMultiAppendUnicodeStringBuffer");
    if (!pRtlMultiAppendUnicodeStringBuffer)
    {
        skip("RtlMultiAppendUnicodeStringBuffer is not exported\n");
        return;
    }

    /* Fits the static buffer: Size stays, MaximumLength = Length + NUL */
    Check("3-into-static", 64, L"", 3, Three, STATUS_SUCCESS, L"abcdef", 64, TRUE);
    Check("exact-fit", 12, L"", 2, Two, STATUS_SUCCESS, L"abcde", 12, TRUE);
    Check("after-initial", 32, L"iii", 2, XyZ, STATUS_SUCCESS, L"iiixyz", 32, TRUE);
    Check("zero-addends", 16, L"", 0, NULL, STATUS_SUCCESS, L"", 16, TRUE);
    Check("empty-addend", 16, L"", 2, EmptyA, STATUS_SUCCESS, L"a", 16, TRUE);
    /* Too small: grows to exactly Length + NUL and keeps the old text */
    Check("grow", 8, L"", 3, Grow, STATUS_SUCCESS, L"abcdefghijkl", 26, FALSE);
    Check("grow-keeps-initial", 8, L"iii", 1, Xyz, STATUS_SUCCESS, L"iiixyz", 14, FALSE);
    TestLimits();
}
