/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RegQueryMultipleValues: ve_valueptr must be a full pointer into the value buffer
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"
#include <winreg.h>

static WCHAR g_SubKeyW[64];
static CHAR g_SubKeyA[64];

static VOID
MakeKeyNames(VOID)
{
    _snwprintf(g_SubKeyW, _countof(g_SubKeyW), L"Software\\LiberNT Tests\\RQMV_%08lx", GetCurrentProcessId());
    _snprintf(g_SubKeyA, _countof(g_SubKeyA), "Software\\LiberNT Tests\\RQMV_%08lx", GetCurrentProcessId());
}

static BOOL
SetupValuesW(HKEY *Key)
{
    HKEY hKey;
    DWORD value;
    LONG ret;

    ret = RegCreateKeyExW(HKEY_CURRENT_USER, g_SubKeyW, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
    ok(ret == ERROR_SUCCESS, "RegCreateKeyExW failed: %ld\n", ret);
    if (ret != ERROR_SUCCESS)
        return FALSE;

    ret = RegSetValueExW(hKey, L"RQMV_One", 0, REG_SZ, (const BYTE *)L"First", sizeof(L"First"));
    ok(ret == ERROR_SUCCESS, "RegSetValueExW(One) failed: %ld\n", ret);

    value = 0x12345678;
    ret = RegSetValueExW(hKey, L"RQMV_Two", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    ok(ret == ERROR_SUCCESS, "RegSetValueExW(Two) failed: %ld\n", ret);

    ret = RegSetValueExW(hKey, L"RQMV_Three", 0, REG_EXPAND_SZ, (const BYTE *)L"%PATH%", sizeof(L"%PATH%"));
    ok(ret == ERROR_SUCCESS, "RegSetValueExW(Three) failed: %ld\n", ret);

    *Key = hKey;
    return TRUE;
}

static VOID
CheckPointerRangeW(VALENTW *Ve, ULONG Count, PBYTE Buffer, DWORD Total)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        ok(Ve[i].ve_valueptr != 0, "ve[%lu].ve_valueptr is NULL\n", i);
        ok(Ve[i].ve_valueptr >= (DWORD_PTR)Buffer &&
           Ve[i].ve_valueptr + Ve[i].ve_valuelen <= (DWORD_PTR)Buffer + Total,
           "ve[%lu].ve_valueptr %p (len %lu) outside buffer %p..%p\n",
           i, (PVOID)Ve[i].ve_valueptr, Ve[i].ve_valuelen,
           Buffer, Buffer + Total);
    }
}

static VOID
TestQueryMultipleValuesW(VOID)
{
    BYTE buffer[256];
    VALENTW ve[3];
    DWORD total;
    DWORD type;
    LONG ret;
    HKEY hKey;

    if (!SetupValuesW(&hKey))
        return;

    ve[0].ve_valuename = L"RQMV_One";
    ve[1].ve_valuename = L"RQMV_Two";
    ve[2].ve_valuename = L"RQMV_Three";

    total = sizeof(buffer);
    ret = RegQueryMultipleValuesW(hKey, ve, 3, (LPWSTR)buffer, &total);
    ok(ret == ERROR_SUCCESS, "RegQueryMultipleValuesW failed: %ld\n", ret);
    if (ret == ERROR_SUCCESS)
    {
        CheckPointerRangeW(ve, 3, buffer, total);

        ok(ve[0].ve_type == REG_SZ, "ve[0].ve_type %lu, expected REG_SZ\n", ve[0].ve_type);
        ok(wcscmp((PWSTR)ve[0].ve_valueptr, L"First") == 0,
           "ve[0] content %ls\n", (PWSTR)ve[0].ve_valueptr);

        ok(ve[1].ve_type == REG_DWORD, "ve[1].ve_type %lu, expected REG_DWORD\n", ve[1].ve_type);
        ok(ve[1].ve_valuelen == sizeof(DWORD), "ve[1].ve_valuelen %lu\n", ve[1].ve_valuelen);
        ok(*(PDWORD)ve[1].ve_valueptr == 0x12345678,
           "ve[1] content 0x%08lx\n", *(PDWORD)ve[1].ve_valueptr);

        type = ve[2].ve_type;
        ok(type == REG_EXPAND_SZ, "ve[2].ve_type %lu, expected REG_EXPAND_SZ\n", type);
        ok(wcscmp((PWSTR)ve[2].ve_valueptr, L"%PATH%") == 0,
           "ve[2] content %ls\n", (PWSTR)ve[2].ve_valueptr);
    }

    RegCloseKey(hKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, g_SubKeyW);
}

static VOID
TestQueryMultipleValuesA(VOID)
{
    BYTE buffer[256];
    VALENTA ve[3];
    DWORD total;
    LONG ret;
    HKEY hKey;

    ret = RegCreateKeyExA(HKEY_CURRENT_USER, g_SubKeyA, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
    ok(ret == ERROR_SUCCESS, "RegCreateKeyExA failed: %ld\n", ret);
    if (ret != ERROR_SUCCESS)
        return;

    ret = RegSetValueExA(hKey, "RQMV_One", 0, REG_SZ, (const BYTE *)"First", sizeof("First"));
    ok(ret == ERROR_SUCCESS, "RegSetValueExA(One) failed: %ld\n", ret);
    ret = RegSetValueExA(hKey, "RQMV_Two", 0, REG_SZ, (const BYTE *)"Second", sizeof("Second"));
    ok(ret == ERROR_SUCCESS, "RegSetValueExA(Two) failed: %ld\n", ret);
    ret = RegSetValueExA(hKey, "RQMV_Three", 0, REG_SZ, (const BYTE *)"Third", sizeof("Third"));
    ok(ret == ERROR_SUCCESS, "RegSetValueExA(Three) failed: %ld\n", ret);

    ve[0].ve_valuename = "RQMV_One";
    ve[1].ve_valuename = "RQMV_Two";
    ve[2].ve_valuename = "RQMV_Three";

    total = sizeof(buffer);
    ret = RegQueryMultipleValuesA(hKey, ve, 3, (LPSTR)buffer, &total);
    ok(ret == ERROR_SUCCESS, "RegQueryMultipleValuesA failed: %ld\n", ret);
    if (ret == ERROR_SUCCESS)
    {
        ULONG i;
        PCSTR expected[3] = { "First", "Second", "Third" };

        for (i = 0; i < 3; i++)
        {
            ok(ve[i].ve_valueptr != 0, "ve[%lu].ve_valueptr is NULL\n", i);
            ok(ve[i].ve_valueptr >= (DWORD_PTR)buffer &&
               ve[i].ve_valueptr + ve[i].ve_valuelen <= (DWORD_PTR)buffer + total,
               "ve[%lu].ve_valueptr %p (len %lu) outside buffer %p..%p\n",
               i, (PVOID)ve[i].ve_valueptr, ve[i].ve_valuelen,
               buffer, buffer + total);
            ok(strcmp((PSTR)ve[i].ve_valueptr, expected[i]) == 0,
               "ve[%lu] content %s\n", i, (PSTR)ve[i].ve_valueptr);
        }
    }

    RegCloseKey(hKey);
    RegDeleteKeyA(HKEY_CURRENT_USER, g_SubKeyA);
}

START_TEST(RegQueryMultipleValues)
{
    MakeKeyNames();
    TestQueryMultipleValuesW();
    TestQueryMultipleValuesA();
}
