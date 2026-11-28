/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A changed file association icon is used after SHCNE_ASSOCCHANGED
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <windows.h>
#include <shlobj.h>

#define TEST_CLASS_KEY L"Software\\Classes\\.lnticontest"
#define TEST_FILE_NAME L"sample.lnticontest"

static LONG SetTestDefaultIcon(LPCWSTR value)
{
    HKEY key;
    LONG error;

    error = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_CLASS_KEY L"\\DefaultIcon", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (error != ERROR_SUCCESS)
        return error;
    error = RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)value, (DWORD)((lstrlenW(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    return error;
}

static int QueryTestIconIndex(void)
{
    SHFILEINFOW info;

    ZeroMemory(&info, sizeof(info));
    if (!SHGetFileInfoW(TEST_FILE_NAME, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return -1;
    return info.iIcon;
}

START_TEST(AssocIconChange)
{
    int first, again, changed;
    LONG error;

    RegDeleteTreeW(HKEY_CURRENT_USER, TEST_CLASS_KEY);
    error = SetTestDefaultIcon(L"shell32.dll,3");
    ok(error == ERROR_SUCCESS, "cannot register the test file type: %ld\n", error);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);

    first = QueryTestIconIndex();
    ok(first >= 0, "no icon for the test file type\n");
    again = QueryTestIconIndex();
    ok(again == first, "icon index changed without an association change: %d then %d\n", first, again);

    error = SetTestDefaultIcon(L"shell32.dll,4");
    ok(error == ERROR_SUCCESS, "cannot change the test file type: %ld\n", error);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);
    changed = QueryTestIconIndex();
    ok(changed >= 0, "no icon for the changed test file type\n");
    ok(changed != first, "the changed DefaultIcon is not used after SHCNE_ASSOCCHANGED: still %d\n", first);

    RegDeleteTreeW(HKEY_CURRENT_USER, TEST_CLASS_KEY);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);
}
