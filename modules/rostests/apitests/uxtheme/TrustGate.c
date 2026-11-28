/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the uxtheme trust gate (ThemeLoader4 I1a-T, THEME-TRUST-POLICY v5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>
#include <strsafe.h>

typedef HRESULT (WINAPI *FN_OpenThemeFile)(LPCWSTR, LPCWSTR, LPCWSTR, HANDLE *, DWORD);
typedef HRESULT (WINAPI *FN_CloseThemeFile)(HANDLE);
typedef HRESULT (WINAPI *FN_ApplyTheme)(HANDLE, UINT, HWND);

static FN_OpenThemeFile pOpenThemeFile;
static FN_CloseThemeFile pCloseThemeFile;
static FN_ApplyTheme pApplyTheme;
static WCHAR g_store[MAX_PATH];

#define GUARD_KEY L"Software\\Leeway\\UxTheme\\Guard\\Themes-Aero"

static HRESULT
OpenClose(LPCWSTR path)
{
    HANDLE tf = NULL;
    HRESULT hr = pOpenThemeFile(path, NULL, NULL, &tf, 0);

    if (SUCCEEDED(hr) && tf)
        pCloseThemeFile(tf);
    return hr;
}

static int
ChildThemeActive(void)
{
    WCHAR self[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 2;

    GetModuleFileNameW(NULL, self, ARRAY_SIZE(self));
    StringCchPrintfW(cmd, ARRAY_SIZE(cmd), L"\"%s\" TrustGate child", self);
    if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static DWORD
GuardValueCount(void)
{
    HKEY key;
    DWORD count = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, GUARD_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return count;
}

static void
SetGuardValue(LPCWSTR name, DWORD type, const void *data, DWORD size)
{
    HKEY key;

    ok(RegCreateKeyExW(HKEY_CURRENT_USER, GUARD_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS,
       "cannot create the guard key\n");
    RegSetValueExW(key, name, 0, type, data, size);
    RegCloseKey(key);
}

static void
ClearGuard(void)
{
    RegDeleteKeyW(HKEY_CURRENT_USER, GUARD_KEY);
}

/* T-LOAD-1/T-STORE-8 and the guard of a clean load: the shipped style loads, and leaves no mark behind */
static void
Test_StorePath(void)
{
    HRESULT hr;

    ClearGuard();
    hr = OpenClose(g_store);
    ok(hr == S_OK, "store style: hr 0x%lx\n", hr);
    ok(GuardValueCount() == 0, "a clean load must remove its own mark, %lu left\n", GuardValueCount());
}

/* 5.9: a v3 style outside the store is refused (the T-GATE-4 rollback is off by default) */
static void
Test_OutsideStore(void)
{
    WCHAR dir[MAX_PATH], copy[MAX_PATH], sub[MAX_PATH], subcopy[MAX_PATH];
    HRESULT hr;

    GetTempPathW(ARRAY_SIZE(dir), dir);
    StringCchPrintfW(copy, ARRAY_SIZE(copy), L"%sAero.msstyles", dir);
    ok(CopyFileW(g_store, copy, FALSE), "copy failed: %lu\n", GetLastError());
    hr = OpenClose(copy);
    ok(hr == E_ACCESSDENIED, "temp copy: hr 0x%lx\n", hr);

    /* the same layout as the store, in a user directory: still refused (equality with the built path) */
    StringCchPrintfW(sub, ARRAY_SIZE(sub), L"%sResources", dir);
    CreateDirectoryW(sub, NULL);
    StringCchCatW(sub, ARRAY_SIZE(sub), L"\\Themes");
    CreateDirectoryW(sub, NULL);
    StringCchCatW(sub, ARRAY_SIZE(sub), L"\\Aero");
    CreateDirectoryW(sub, NULL);
    StringCchPrintfW(subcopy, ARRAY_SIZE(subcopy), L"%s\\Aero.msstyles", sub);
    CopyFileW(g_store, subcopy, FALSE);
    hr = OpenClose(subcopy);
    ok(hr == E_ACCESSDENIED, "store-like user path: hr 0x%lx\n", hr);

    /* a store path spelled with a relative component, a stream, or a short name is not the built path */
    StringCchPrintfW(copy, ARRAY_SIZE(copy), L"%s\\..\\Themes\\Aero\\Aero.msstyles", g_store);
    hr = OpenClose(copy);
    ok(FAILED(hr), "path with ..: hr 0x%lx\n", hr);
    StringCchPrintfW(copy, ARRAY_SIZE(copy), L"%s:stream", g_store);
    hr = OpenClose(copy);
    ok(FAILED(hr), "stream path: hr 0x%lx\n", hr);
    hr = OpenClose(L"Aero.msstyles");
    ok(FAILED(hr), "relative name: hr 0x%lx\n", hr);
    DeleteFileW(subcopy);
}

/* T-GUARD-2: a mark of a process that no longer exists makes the style skip; so does a malformed mark */
static void
Test_Guard(void)
{
    struct { DWORD version, reserved; ULONGLONG at; } data = { 1, 0, 0 };
    HRESULT hr;

    ClearGuard();
    SetGuardValue(L"FFFFFFF0-0000000000000001", REG_BINARY, &data, sizeof(data));
    ok(ChildThemeActive() == 0, "a process's own load must skip a style with a stale mark\n");
    hr = OpenClose(g_store);
    ok(hr == S_OK, "a load the user asks for does not skip: hr 0x%lx\n", hr);
    ok(GuardValueCount() == 1, "the stale mark must stay, count %lu\n", GuardValueCount());
    if (pApplyTheme)
    {
        HANDLE tf = NULL;

        hr = pOpenThemeFile(g_store, NULL, NULL, &tf, 0);
        ok(hr == S_OK, "open for apply: hr 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = pApplyTheme(tf, 0, NULL);
            ok(hr == S_OK, "ApplyTheme: hr 0x%lx\n", hr);
            ok(GuardValueCount() == 0, "ApplyTheme must clear the marks, count %lu\n", GuardValueCount());
            pCloseThemeFile(tf);
        }
        ok(ChildThemeActive() == 1, "after ApplyTheme the own load succeeds\n");
    }

    ClearGuard();
    SetGuardValue(L"00000001-0000000000000001", REG_SZ, L"x", 4);
    ok(ChildThemeActive() == 0, "wrong type must skip the own load\n");

    ClearGuard();
    data.version = 2;
    SetGuardValue(L"00000001-0000000000000001", REG_BINARY, &data, sizeof(data));
    ok(ChildThemeActive() == 0, "wrong version must skip the own load\n");

    ClearGuard();
    SetGuardValue(L"not-a-mark", REG_BINARY, &data, sizeof(data));
    ok(ChildThemeActive() == 0, "malformed name must skip the own load\n");
    SetGuardValue(L" 0000001-0000000000000001", REG_BINARY, &data, sizeof(data));
    ok(ChildThemeActive() == 0, "a name with a leading space is malformed\n");

    ClearGuard();
    hr = OpenClose(g_store);
    ok(hr == S_OK, "after clearing: hr 0x%lx\n", hr);
}

/* T-LOAD-2: a writer holding the style open is a clean refusal that writes no mark */
static void
Test_SharingViolation(void)
{
    HANDLE h;
    HRESULT hr;

    /* the store is not writable for this test, so the other party is a read handle that shares nothing */
    ClearGuard();
    h = CreateFileW(g_store, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        skip("cannot open the store style exclusively: %lu\n", GetLastError());
        return;
    }
    hr = OpenClose(g_store);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "sharing violation: hr 0x%lx\n", hr);
    ok(GuardValueCount() == 0, "no mark on a sharing violation, count %lu\n", GuardValueCount());
    CloseHandle(h);
}

START_TEST(TrustGate)
{
    HMODULE mod;
    WCHAR windir[MAX_PATH];
    char **argv;

    if (winetest_get_mainargs(&argv) >= 3 && !strcmp(argv[2], "child"))
    {
        typedef BOOL (WINAPI *FN_IsThemeActive)(void);
        FN_IsThemeActive pIsThemeActive;

        mod = LoadLibraryW(L"uxtheme.dll");
        pIsThemeActive = (FN_IsThemeActive)GetProcAddress(mod, "IsThemeActive");
        ExitProcess(pIsThemeActive && pIsThemeActive() ? 1 : 0);
    }
    mod = LoadLibraryW(L"uxtheme.dll");

    pOpenThemeFile = (FN_OpenThemeFile)GetProcAddress(mod, MAKEINTRESOURCEA(2));
    pCloseThemeFile = (FN_CloseThemeFile)GetProcAddress(mod, MAKEINTRESOURCEA(3));
    pApplyTheme = (FN_ApplyTheme)GetProcAddress(mod, MAKEINTRESOURCEA(4));
    if (!pOpenThemeFile || !pCloseThemeFile)
    {
        skip("OpenThemeFile / CloseThemeFile not exported\n");
        return;
    }
    GetSystemWindowsDirectoryW(windir, ARRAY_SIZE(windir));
    StringCchPrintfW(g_store, ARRAY_SIZE(g_store), L"%s\\Resources\\Themes\\Aero\\Aero.msstyles", windir);
    if (GetFileAttributesW(g_store) == INVALID_FILE_ATTRIBUTES)
    {
        skip("no shipped style at %ls\n", g_store);
        return;
    }
    Test_StorePath();
    Test_OutsideStore();
    Test_Guard();
    Test_SharingViolation();
    ClearGuard();
}
