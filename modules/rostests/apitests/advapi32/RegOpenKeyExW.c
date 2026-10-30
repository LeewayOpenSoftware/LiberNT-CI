/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the RegOpenKeyExW alignment
 * PROGRAMMER:      Mark Jansen (mark.jansen@reactos.org)
 *                  Ahmed ARIF (arif.ing@outlook.com)
 */

#include "precomp.h"

#define TEST_STR    L".exe"
#define VIEW_TEST_KEY L"Software\\LiberNTRegOpenKeyExWViewTest"

static void Test_ViewFlagsWithWriteAccess(void)
{
    HKEY hk, hkSub;
    LONG lRes;

    lRes = RegCreateKeyExW(HKEY_CURRENT_USER, VIEW_TEST_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL);
    ok_int(lRes, ERROR_SUCCESS);
    if (lRes)
        return;
    RegCloseKey(hk);

    lRes = RegOpenKeyExW(HKEY_CURRENT_USER, VIEW_TEST_KEY, 0, KEY_READ | KEY_WRITE | KEY_WOW64_32KEY, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    lRes = RegOpenKeyExW(HKEY_CURRENT_USER, VIEW_TEST_KEY, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    lRes = RegOpenKeyExW(HKEY_CURRENT_USER, VIEW_TEST_KEY L"\\Missing", 0, KEY_READ | KEY_WOW64_32KEY, &hkSub);
    ok_int(lRes, ERROR_FILE_NOT_FOUND);
    if (!lRes)
        RegCloseKey(hkSub);

    lRes = RegDeleteKeyW(HKEY_CURRENT_USER, VIEW_TEST_KEY);
    ok_int(lRes, ERROR_SUCCESS);
}

static void Test_ViewFlagsInAppHive(void)
{
    WCHAR Path[MAX_PATH];
    HKEY hkApp, hk;
    LONG lRes;

    GetTempPathW(MAX_PATH, Path);
    wcscat(Path, L"LiberNTRegOpenKeyExWAppHive.dat");
    DeleteFileW(Path);

    lRes = RegLoadAppKeyW(Path, &hkApp, KEY_ALL_ACCESS, 0, 0);
    ok_int(lRes, ERROR_SUCCESS);
    if (lRes)
        return;

    lRes = RegCreateKeyExW(hkApp, L"Software\\Sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    lRes = RegOpenKeyExW(hkApp, L"Software\\Sub", 0, KEY_READ | KEY_WOW64_32KEY, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    lRes = RegOpenKeyExW(hkApp, L"Software\\Sub", 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    lRes = RegOpenKeyExW(hkApp, L"Software\\Missing", 0, KEY_READ | KEY_WOW64_32KEY, &hk);
    ok_int(lRes, ERROR_FILE_NOT_FOUND);
    if (!lRes)
        RegCloseKey(hk);

    RegCloseKey(hkApp);
    DeleteFileW(Path);
}

START_TEST(RegOpenKeyExW)
{
    char GccShouldNotAlignThis[20 * 2];
    char GccShouldNotAlignThis2[20];
    PVOID Alias = GccShouldNotAlignThis + 1;
    PVOID UnalignedKey = GccShouldNotAlignThis2 + 1;
    HKEY hk;
    LONG lRes;

    memcpy(Alias, TEST_STR, sizeof(TEST_STR));

    lRes = RegOpenKeyExW(HKEY_CLASSES_ROOT, TEST_STR, 0, KEY_READ, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (lRes)
        return;
    RegCloseKey(hk);

    ok_hex(((ULONG_PTR)Alias) % 2, 1);
    lRes = RegOpenKeyExW(HKEY_CLASSES_ROOT, Alias, 0, KEY_READ, &hk);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
        RegCloseKey(hk);

    ok_hex(((ULONG_PTR)UnalignedKey) % 2, 1);
    lRes = RegOpenKeyExW(HKEY_CLASSES_ROOT, TEST_STR, 0, KEY_READ, UnalignedKey);
    ok_int(lRes, ERROR_SUCCESS);
    if (!lRes)
    {
        RegCloseKey(*(HKEY*)(UnalignedKey));
    }

    Test_ViewFlagsWithWriteAccess();
    Test_ViewFlagsInAppHive();
}
