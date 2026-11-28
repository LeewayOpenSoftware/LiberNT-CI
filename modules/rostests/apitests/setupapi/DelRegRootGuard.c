/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     An INF DelReg entry with an empty subkey must not delete a predefined
 *              root key, while a relative (HKR) root keeps its Windows delete semantics.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>

/*
 * Regression guard for the predefined-root DelReg fix (dll/win32/setupapi/install.c,
 * do_reg_operation). Two sections:
 *   [Del.Root]   "HKCU,,"  - an empty-subkey DelReg on a predefined root. The fix makes
 *                            this a no-op (Windows never deletes a predefined root here).
 *   [Del.RootBs] "HKCU,\"  - a backslash-only subkey: it opens the root through a real
 *                            handle, so the decision must come from the subkey text, not
 *                            the opened handle. Also a no-op after the fix.
 *   [Del.Hkr]    "HKR,,"   - the same shape on a relative root, which the fix must NOT
 *                            change: HKR keeps its (pre-existing) Windows delete semantics.
 *
 * The HKCU case is non-destructive BY CONSTRUCTION: before running it the test redirects
 * HKEY_CURRENT_USER to a private scratch key with RegOverridePredefKey. setupapi opens the
 * empty-subkey root through RegOpenKeyW, which returns the predefined handle unchanged (so
 * the fix's is_predefined_root() check still fires); every delete it then performs -
 * RegDeleteTreeW(root, NULL) - resolves the predefined handle through MapDefaultKey and so
 * hits the scratch key, never the real hive, and NtDeleteKey() on the pseudo-handle fails
 * harmlessly. On a build WITHOUT the fix only the scratch key's children are cleared (the
 * sentinel disappears = a clean FAIL); the user's real HKCU is never touched. If the
 * override could not be installed the test skips the destructive run rather than risk it.
 */

#define TEST_BASE     L"Software\\LntDelRegGuardTest"
#define TEST_SCRATCH  L"Software\\LntDelRegGuardTest\\Scratch"
#define TEST_HKRROOT  L"Software\\LntDelRegGuardTest\\HkrRoot"

static const char TestInfData[] =
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "\r\n"
    "[DelRootSection]\r\n"
    "DelReg=Del.Root\r\n"
    "\r\n"
    "[Del.Root]\r\n"
    "HKCU,,\r\n"
    "\r\n"
    "[DelRootBsSection]\r\n"
    "DelReg=Del.RootBs\r\n"
    "\r\n"
    "[Del.RootBs]\r\n"
    "HKCU,\"\\\"\r\n"
    "\r\n"
    "[DelHkrSection]\r\n"
    "DelReg=Del.Hkr\r\n"
    "\r\n"
    "[Del.Hkr]\r\n"
    "HKR,,\r\n";

static BOOL write_file(PCWSTR Path, const char *Data, DWORD Len)
{
    HANDLE h = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL wrote;

    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    wrote = WriteFile(h, Data, Len, &written, NULL);
    CloseHandle(h);
    return wrote && written == Len;
}

static BOOL key_exists(HKEY Root, PCWSTR SubKey)
{
    HKEY hKey;
    if (RegOpenKeyExW(Root, SubKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return TRUE;
    }
    return FALSE;
}

/* The predefined-root case, redirected onto a private scratch key. */
static void TestPredefinedRoot(HINF hInf)
{
    HKEY hBase = NULL, hScratch = NULL, hSentinel = NULL;
    LONG rc, ov;
    BOOL ran;

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_BASE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hBase, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(base) failed %ld\n", rc);
    if (rc != ERROR_SUCCESS)
        return;

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_SCRATCH, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hScratch, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(scratch) failed %ld\n", rc);
    if (rc != ERROR_SUCCESS)
        goto cleanup;

    /* Redirect HKCU onto the scratch key for the duration of the destructive run. */
    ov = RegOverridePredefKey(HKEY_CURRENT_USER, hScratch);
    ok(ov == ERROR_SUCCESS, "RegOverridePredefKey(HKCU) failed %ld\n", ov);
    if (ov != ERROR_SUCCESS)
    {
        skip("Could not redirect HKCU; not running the destructive DelReg\n");
        goto cleanup;
    }

    /* Written through the override -> lands under the scratch key. */
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Sentinel", 0, NULL, 0, KEY_WRITE, NULL, &hSentinel, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(sentinel) failed %ld\n", rc);
    if (hSentinel)
        RegCloseKey(hSentinel);

    ran = SetupInstallFromInfSectionW(NULL, hInf, L"DelRootSection", SPINST_REGISTRY,
                                      NULL, NULL, 0, NULL, NULL, NULL, NULL);
    ok(ran, "SetupInstallFromInfSectionW(DelRootSection) failed (%lu)\n", GetLastError());

    /* With the fix the empty-subkey root DelReg is a no-op and the sentinel survives. */
    ok(key_exists(HKEY_CURRENT_USER, L"Sentinel"),
       "HKCU was cleared by an empty-subkey root DelReg - the predefined-root guard is missing\n");

    /* A backslash-only subkey ("HKCU,\") opens the root through a real handle, so a guard
       that only compares the opened handle is bypassed; the subkey text must be checked. */
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Sentinel", 0, NULL, 0, KEY_WRITE, NULL, &hSentinel, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(sentinel 2) failed %ld\n", rc);
    if (hSentinel)
        RegCloseKey(hSentinel);

    ran = SetupInstallFromInfSectionW(NULL, hInf, L"DelRootBsSection", SPINST_REGISTRY,
                                      NULL, NULL, 0, NULL, NULL, NULL, NULL);
    ok(ran, "SetupInstallFromInfSectionW(DelRootBsSection) failed (%lu)\n", GetLastError());

    ok(key_exists(HKEY_CURRENT_USER, L"Sentinel"),
       "HKCU was cleared by a backslash-only root DelReg - the guard checks the handle, not the subkey\n");

    /* Restore the mapping (this closes the mapped scratch handle). */
    RegOverridePredefKey(HKEY_CURRENT_USER, NULL);
    hScratch = NULL;

cleanup:
    if (hScratch)
        RegCloseKey(hScratch);
    if (hBase)
        RegCloseKey(hBase);
    /* Remove the whole private subtree from the real hive. */
    RegDeleteTreeW(HKEY_CURRENT_USER, TEST_BASE);
}

/* The relative-root case: HKR keeps its Windows delete semantics (the guard must not fire). */
static void TestRelativeRoot(HINF hInf)
{
    HKEY hHkr = NULL, hChild = NULL;
    LONG rc;
    BOOL ran;

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_HKRROOT, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hHkr, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(hkrroot) failed %ld\n", rc);
    if (rc != ERROR_SUCCESS)
        return;

    rc = RegCreateKeyExW(hHkr, L"Child", 0, NULL, 0, KEY_WRITE, NULL, &hChild, NULL);
    ok(rc == ERROR_SUCCESS, "RegCreateKeyExW(child) failed %ld\n", rc);
    if (hChild)
        RegCloseKey(hChild);

    ran = SetupInstallFromInfSectionW(NULL, hInf, L"DelHkrSection", SPINST_REGISTRY,
                                      hHkr, NULL, 0, NULL, NULL, NULL, NULL);
    ok(ran, "SetupInstallFromInfSectionW(DelHkrSection) failed (%lu)\n", GetLastError());

    /* HKR is not a predefined root, so the guard does not fire and the relative key's
       child is deleted, exactly as before the fix. */
    ok(!key_exists(hHkr, L"Child"),
       "HKR empty-subkey DelReg did not delete the relative key's child - Windows semantics changed\n");

    RegCloseKey(hHkr);
    RegDeleteTreeW(HKEY_CURRENT_USER, TEST_BASE);
}

START_TEST(DelRegRootGuard)
{
    WCHAR TempDir[MAX_PATH];
    WCHAR InfPath[MAX_PATH] = L"";
    HINF hInf;

    if (!GetTempPathW(ARRAYSIZE(TempDir), TempDir))
    {
        skip("GetTempPathW failed (%lu)\n", GetLastError());
        return;
    }
    lstrcpyW(InfPath, TempDir);
    lstrcatW(InfPath, L"lnt_delregroot.inf");
    if (!write_file(InfPath, TestInfData, (DWORD)(sizeof(TestInfData) - 1)))
    {
        skip("Could not write the test INF (%lu)\n", GetLastError());
        return;
    }

    hInf = SetupOpenInfFileW(InfPath, NULL, INF_STYLE_WIN4, NULL);
    ok(hInf != INVALID_HANDLE_VALUE, "SetupOpenInfFileW failed (%lu)\n", GetLastError());
    if (hInf != INVALID_HANDLE_VALUE)
    {
        TestPredefinedRoot(hInf);
        TestRelativeRoot(hInf);
        SetupCloseInfFile(hInf);
    }

    DeleteFileW(InfPath);
}
