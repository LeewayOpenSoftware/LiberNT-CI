/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for RtlConvertLCIDToString, rows measured on Windows 11 26100
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *FN_RtlConvertLCIDToString)(LCID, ULONG, ULONG, PWSTR, ULONG);

static const struct
{
    LCID Lcid;
    ULONG Base;
    ULONG Padding;
    ULONG Size;
    NTSTATUS Status;
    PCWSTR Expected;    /* NULL: the buffer must be untouched */
} Rows[] =
{
    { 0x409, 16, 4, 16, STATUS_SUCCESS, L"0409" },
    { 0x409, 16, 8, 16, STATUS_SUCCESS, L"00000409" },
    { 0x409, 10, 4, 16, STATUS_SUCCESS, L"1033" },
    { 0x409, 10, 0, 16, STATUS_SUCCESS, L"1033" },
    { 0x409, 16, 0, 16, STATUS_SUCCESS, L"409" },
    { 0x409, 16, 1, 16, STATUS_SUCCESS, L"409" },
    { 0x409, 16, 9, 16, STATUS_SUCCESS, L"000000409" },
    { 0x409,  2, 0, 32, STATUS_SUCCESS, L"10000001001" },
    { 0x409,  8, 0, 16, STATUS_SUCCESS, L"2011" },
    { 0x409,  0, 0, 16, STATUS_SUCCESS, L"1033" },
    { 0x80A, 16, 0, 16, STATUS_SUCCESS, L"80A" },
    { 0x80A, 10, 6, 16, STATUS_SUCCESS, L"002058" },
    { 0xABCDEF, 16, 0, 16, STATUS_SUCCESS, L"ABCDEF" },
    { 0xFFFFFFFF, 10, 0, 16, STATUS_SUCCESS, L"4294967295" },
    { 0xFFFFFFFF, 16, 0, 16, STATUS_SUCCESS, L"FFFFFFFF" },
    { 0, 16, 4, 16, STATUS_SUCCESS, L"0000" },
    /* Exact fits */
    { 0x409, 16, 0, 4, STATUS_SUCCESS, L"409" },
    { 0x409, 16, 4, 5, STATUS_SUCCESS, L"0409" },
    { 0x409, 16, 8, 9, STATUS_SUCCESS, L"00000409" },
    { 0x409, 10, 4, 5, STATUS_SUCCESS, L"1033" },
    { 0x409, 16, 4, 511, STATUS_SUCCESS, L"0409" },
    /* Padding that does not fit is truncated */
    { 0x409, 16, 4, 4, STATUS_UNSUCCESSFUL, L"040" },
    { 0x409, 16, 8, 8, STATUS_UNSUCCESSFUL, L"0000040" },
    { 0x409, 16, 5, 5, STATUS_UNSUCCESSFUL, L"0040" },
    { 0x409, 16, 6, 4, STATUS_UNSUCCESSFUL, L"000" },
    { 0x409, 10, 8, 6, STATUS_UNSUCCESSFUL, L"00001" },
    /* Digits that do not fit, and sizes of 512 or more */
    { 0x409, 16, 0, 3, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 10, 4, 4, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 10, 4, 2, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409,  2, 16, 10, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 16, 4, 1, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 16, 4, 0, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 16, 4, 512, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409, 16, 4, 65536, STATUS_BUFFER_OVERFLOW, NULL },
    { 0x409,  7, 0, 600, STATUS_BUFFER_OVERFLOW, NULL },
    /* Bases */
    { 0x409,  1, 0, 16, STATUS_INVALID_PARAMETER, NULL },
    { 0x409,  7, 0, 16, STATUS_INVALID_PARAMETER, NULL },
    { 0x409,  7, 0, 0, STATUS_INVALID_PARAMETER, NULL },
    { 0x409, 17, 0, 16, STATUS_INVALID_PARAMETER, NULL },
    { 0x409, 36, 0, 1, STATUS_INVALID_PARAMETER, NULL },
};

START_TEST(RtlConvertLCIDToString)
{
    FN_RtlConvertLCIDToString pRtlConvertLCIDToString;
    static WCHAR Buffer[1024];
    NTSTATUS Status;
    ULONG Row, Index;
    BOOLEAN Untouched;

    pRtlConvertLCIDToString = (FN_RtlConvertLCIDToString)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                        "RtlConvertLCIDToString");
    if (!pRtlConvertLCIDToString)
    {
        skip("RtlConvertLCIDToString is not exported\n");
        return;
    }

    for (Row = 0; Row < ARRAYSIZE(Rows); Row++)
    {
        for (Index = 0; Index < ARRAYSIZE(Buffer); Index++)
            Buffer[Index] = L'#';
        Status = pRtlConvertLCIDToString(Rows[Row].Lcid, Rows[Row].Base, Rows[Row].Padding,
                                         Buffer, Rows[Row].Size);
        ok(Status == Rows[Row].Status, "row %lu: status 0x%lx, expected 0x%lx\n",
           Row, Status, Rows[Row].Status);
        if (Rows[Row].Expected)
        {
            ok(!wcscmp(Buffer, Rows[Row].Expected), "row %lu: '%.40ls', expected '%ls'\n",
               Row, Buffer, Rows[Row].Expected);
            ok(Buffer[wcslen(Rows[Row].Expected) + 1] == L'#', "row %lu: wrote past the NUL\n", Row);
        }
        else
        {
            Untouched = TRUE;
            for (Index = 0; Index < 64; Index++)
                Untouched &= (Buffer[Index] == L'#');
            ok(Untouched, "row %lu: buffer was written\n", Row);
        }
    }

    /* Long padding fills exactly Padding characters */
    Status = pRtlConvertLCIDToString(0x409, 16, 200, Buffer, 256);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(wcslen(Buffer), 200);
    ok(!wcscmp(Buffer + 196, L"0409"), "tail '%ls'\n", Buffer + 196);
    Status = pRtlConvertLCIDToString(0x409, 16, 64, Buffer, 64);
    ok_ntstatus(Status, STATUS_UNSUCCESSFUL);
    ok_size_t(wcslen(Buffer), 63);
}
