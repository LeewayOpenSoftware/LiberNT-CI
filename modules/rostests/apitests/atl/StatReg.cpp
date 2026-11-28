/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the ATL script registrar: Delete and ForceRemove of keys that have siblings
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <atlbase.h>
#include <shlwapi.h>

#ifdef HAVE_APITEST
    #include <apitest.h>
#else
    #include "atltest.h"
#endif

#define TEST_ROOT   L"Software\\AtlStatRegTest"
#define TEST_PARENT TEST_ROOT L"\\Parent"

static BOOL
KeyExists(LPCWSTR path)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(key);
    return TRUE;
}

/* Read a REG_SZ; name NULL is the default value */
static LONG
ReadValue(LPCWSTR path, LPCWSTR name, WCHAR *value, DWORD count)
{
    HKEY key;
    DWORD size = count * sizeof(WCHAR), type;
    LONG ret = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key);
    if (ret != ERROR_SUCCESS)
        return ret;
    ret = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)value, &size);
    RegCloseKey(key);
    return ret;
}

static void
WriteValue(LPCWSTR path, LPCWSTR name, LPCWSTR value)
{
    HKEY key;
    LONG ret = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    ok_long(ret, ERROR_SUCCESS);
    if (ret != ERROR_SUCCESS)
        return;
    ret = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
    ok_long(ret, ERROR_SUCCESS);
    RegCloseKey(key);
}

static void
Populate(void)
{
    WriteValue(TEST_PARENT L"\\Sibling", NULL, L"keep");
    WriteValue(TEST_PARENT L"\\Target", NULL, L"old");
    WriteValue(TEST_PARENT L"\\Target", L"Stale", L"gone");
    WriteValue(TEST_PARENT L"\\Target\\Sub", NULL, L"stale subkey");
}

START_TEST(StatReg)
{
    static const WCHAR ForceRemoveScript[] =
        L"HKCU { NoRemove Software { NoRemove AtlStatRegTest { NoRemove Parent {"
        L" ForceRemove Target = s 'new' { } } } } }";
    static const WCHAR DeleteScript[] =
        L"HKCU { NoRemove Software { NoRemove AtlStatRegTest { NoRemove Parent {"
        L" Delete Target } } } }";
    CRegObject registrar;
    WCHAR value[64];
    HRESULT hr;

    SHDeleteKeyW(HKEY_CURRENT_USER, TEST_ROOT);

    /* ForceRemove must replace an existing key even though its parent has other subkeys */
    Populate();
    hr = registrar.StringRegister(ForceRemoveScript);
    ok_hex(hr, S_OK);
    ok_long(ReadValue(TEST_PARENT L"\\Target", NULL, value, _countof(value)), ERROR_SUCCESS);
    ok(!wcscmp(value, L"new"), "ForceRemove default is '%ls'\n", value);
    ok(!KeyExists(TEST_PARENT L"\\Target\\Sub"), "ForceRemove left the old subkey\n");
    ok(ReadValue(TEST_PARENT L"\\Target", L"Stale", value, _countof(value)) != ERROR_SUCCESS,
       "ForceRemove left the old value\n");
    ok_long(ReadValue(TEST_PARENT L"\\Sibling", NULL, value, _countof(value)), ERROR_SUCCESS);
    ok(!wcscmp(value, L"keep"), "Sibling default is '%ls'\n", value);

    /* Delete must remove the key and leave its siblings */
    Populate();
    hr = registrar.StringRegister(DeleteScript);
    ok_hex(hr, S_OK);
    ok(!KeyExists(TEST_PARENT L"\\Target"), "Delete left the key\n");
    ok_long(ReadValue(TEST_PARENT L"\\Sibling", NULL, value, _countof(value)), ERROR_SUCCESS);
    ok(!wcscmp(value, L"keep"), "Sibling default is '%ls' after Delete\n", value);

    SHDeleteKeyW(HKEY_CURRENT_USER, TEST_ROOT);
}
