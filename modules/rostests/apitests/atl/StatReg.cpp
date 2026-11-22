/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the ATL script registrar: Delete and ForceRemove of keys that have siblings
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <atlbase.h>

#ifdef HAVE_APITEST
    #include <apitest.h>
#else
    #include "atltest.h"
#endif

static LONG
ReadDefault(HKEY root, LPCWSTR path, WCHAR *value, DWORD count)
{
    CRegKey key;
    LONG ret = key.Open(root, path, KEY_READ);
    if (ret != ERROR_SUCCESS)
        return ret;
    ULONG size = count;
    return key.QueryStringValue(NULL, value, &size);
}

static BOOL
KeyExists(HKEY root, LPCWSTR path)
{
    CRegKey key;
    return key.Open(root, path, KEY_READ) == ERROR_SUCCESS;
}

static void
Populate(void)
{
    CRegKey parent, key;
    ok_long(parent.Create(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent"), ERROR_SUCCESS);
    ok_long(key.Create(parent, L"Sibling"), ERROR_SUCCESS);
    ok_long(key.SetStringValue(NULL, L"keep"), ERROR_SUCCESS);
    key.Close();
    ok_long(key.Create(parent, L"Target"), ERROR_SUCCESS);
    ok_long(key.SetStringValue(NULL, L"old"), ERROR_SUCCESS);
    ok_long(key.SetStringValue(L"Stale", L"gone"), ERROR_SUCCESS);
    CRegKey sub;
    ok_long(sub.Create(key, L"Sub"), ERROR_SUCCESS);
    ok_long(sub.SetStringValue(NULL, L"stale subkey"), ERROR_SUCCESS);
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

    CRegKey root;
    root.Open(HKEY_CURRENT_USER, L"Software", KEY_READ | KEY_WRITE);
    root.RecurseDeleteKey(L"AtlStatRegTest");
    root.Close();

    /* ForceRemove must replace an existing key even though its parent has other subkeys */
    Populate();
    hr = registrar.StringRegister(ForceRemoveScript);
    ok_hex(hr, S_OK);
    ok_long(ReadDefault(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Target", value, _countof(value)),
            ERROR_SUCCESS);
    ok(!wcscmp(value, L"new"), "ForceRemove default is '%ls'\n", value);
    ok(!KeyExists(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Target\\Sub"),
       "ForceRemove left the old subkey\n");
    {
        CRegKey key;
        WCHAR stale[16];
        ULONG size = _countof(stale);
        ok_long(key.Open(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Target", KEY_READ), ERROR_SUCCESS);
        ok(key.QueryStringValue(L"Stale", stale, &size) != ERROR_SUCCESS, "ForceRemove left the old value\n");
    }
    ok_long(ReadDefault(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Sibling", value, _countof(value)),
            ERROR_SUCCESS);
    ok(!wcscmp(value, L"keep"), "Sibling default is '%ls'\n", value);

    /* Delete must remove the key and leave its siblings */
    Populate();
    hr = registrar.StringRegister(DeleteScript);
    ok_hex(hr, S_OK);
    ok(!KeyExists(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Target"), "Delete left the key\n");
    ok_long(ReadDefault(HKEY_CURRENT_USER, L"Software\\AtlStatRegTest\\Parent\\Sibling", value, _countof(value)),
            ERROR_SUCCESS);
    ok(!wcscmp(value, L"keep"), "Sibling default is '%ls' after Delete\n", value);

    root.Open(HKEY_CURRENT_USER, L"Software", KEY_READ | KEY_WRITE);
    root.RecurseDeleteKey(L"AtlStatRegTest");
}
