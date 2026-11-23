/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetOpenFileName/GetSaveFileName routing to the item dialog
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <commdlg.h>
#include <cderr.h>
#include <dlgs.h>

#include "wine/test.h"

#define ROUTE_TITLE L"comdlg32 route test"
#define ROUTE_TITLE_A "comdlg32 route test"

static const WCHAR PolicyKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Comdlg32";

struct driver
{
    const WCHAR *text;
    DWORD thread;
    BOOL found;
    BOOL item;
};

static BOOL CALLBACK find_dialog(HWND hwnd, LPARAM lparam)
{
    WCHAR name[16];

    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, name, ARRAY_SIZE(name)) && !lstrcmpW(name, L"#32770"))
    {
        *(HWND *)lparam = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* Finds the dialog of the test thread, records which dialog it is, then accepts or cancels it. */
static DWORD WINAPI driver_proc(void *arg)
{
    struct driver *driver = arg;
    HWND hwnd = NULL;
    int i;

    for (i = 0; i < 400 && !hwnd; i++)
    {
        EnumThreadWindows(driver->thread, find_dialog, (LPARAM)&hwnd);
        if (!hwnd)
            Sleep(50);
    }
    if (!hwnd)
        return 0;
    Sleep(300);
    driver->found = TRUE;
    /* The classic dialog has the "Look in" combo box; the item dialog does not. */
    driver->item = GetDlgItem(hwnd, cmb2) == NULL;
    if (driver->text)
    {
        SendDlgItemMessageW(hwnd, edt1, WM_SETTEXT, 0, (LPARAM)driver->text);
        PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
    }
    else
    {
        PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
    }
    return 0;
}

static HANDLE start_driver(struct driver *driver, const WCHAR *text)
{
    ZeroMemory(driver, sizeof(*driver));
    driver->text = text;
    driver->thread = GetCurrentThreadId();
    return CreateThread(NULL, 0, driver_proc, driver, 0, NULL);
}

static void finish_driver(HANDLE thread)
{
    ok(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0, "driver thread did not finish\n");
    CloseHandle(thread);
}

static void init_ofn(OPENFILENAMEW *ofn, WCHAR *file, DWORD size, const WCHAR *dir)
{
    ZeroMemory(ofn, sizeof(*ofn));
    ofn->lStructSize = sizeof(*ofn);
    ofn->lpstrFile = file;
    ofn->nMaxFile = size;
    ofn->lpstrTitle = ROUTE_TITLE;
    ofn->lpstrFilter = L"Text files\0*.txt\0All files\0*.*\0";
    ofn->nFilterIndex = 1;
    ofn->lpstrInitialDir = dir;
    ofn->Flags = OFN_EXPLORER;
}

static void get_test_dir(WCHAR *dir)
{
    WCHAR temp[MAX_PATH];

    GetTempPathW(ARRAY_SIZE(temp), temp);
    lstrcatW(temp, L"cdlgroute");
    CreateDirectoryW(temp, NULL);
    /* The item dialog returns long names; TEMP may be an 8.3 path. */
    if (!GetLongPathNameW(temp, dir, MAX_PATH))
        lstrcpyW(dir, temp);
}

static void create_file(const WCHAR *dir, const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    HANDLE file;

    lstrcpyW(path, dir);
    lstrcatW(path, L"\\");
    lstrcatW(path, name);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "cannot create %s\n", wine_dbgstr_w(path));
    CloseHandle(file);
}

static void delete_file(const WCHAR *dir, const WCHAR *name)
{
    WCHAR path[MAX_PATH];

    lstrcpyW(path, dir);
    lstrcatW(path, L"\\");
    lstrcatW(path, name);
    DeleteFileW(path);
}

static void test_save_ok(const WCHAR *dir)
{
    WCHAR file[MAX_PATH] = L"", expected[MAX_PATH], cwd[MAX_PATH], saved[MAX_PATH];
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    BOOL ret;

    GetCurrentDirectoryW(ARRAY_SIZE(saved), saved);
    delete_file(dir, L"routesave.txt");
    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    ofn.Flags |= OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"txt";
    thread = start_driver(&driver, L"routesave");
    ret = GetSaveFileNameW(&ofn);
    finish_driver(thread);

    ok(driver.found, "dialog not found\n");
    ok(driver.item, "expected the item dialog\n");
    ok(ret, "GetSaveFileNameW failed, error %#lx\n", CommDlgExtendedError());
    lstrcpyW(expected, dir);
    lstrcatW(expected, L"\\routesave.txt");
    ok(!lstrcmpiW(file, expected), "got %s, expected %s\n", wine_dbgstr_w(file), wine_dbgstr_w(expected));
    ok(ofn.nFileOffset == lstrlenW(dir) + 1, "nFileOffset %u\n", ofn.nFileOffset);
    ok(ofn.nFileExtension == lstrlenW(expected) - 3, "nFileExtension %u\n", ofn.nFileExtension);
    ok(ofn.nFilterIndex == 1, "nFilterIndex %lu\n", ofn.nFilterIndex);
    GetCurrentDirectoryW(ARRAY_SIZE(cwd), cwd);
    ok(!lstrcmpiW(cwd, dir), "current directory %s, expected %s\n", wine_dbgstr_w(cwd), wine_dbgstr_w(dir));
    SetCurrentDirectoryW(saved);

    /* OFN_NOCHANGEDIR keeps the current directory. */
    file[0] = 0;
    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    ofn.Flags |= OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"txt";
    thread = start_driver(&driver, L"routesave2");
    ret = GetSaveFileNameW(&ofn);
    finish_driver(thread);
    ok(ret, "GetSaveFileNameW failed, error %#lx\n", CommDlgExtendedError());
    GetCurrentDirectoryW(ARRAY_SIZE(cwd), cwd);
    ok(!lstrcmpiW(cwd, saved), "current directory changed to %s\n", wine_dbgstr_w(cwd));
}

static void test_open_cancel(const WCHAR *dir)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    BOOL ret;

    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    ofn.Flags |= OFN_FILEMUSTEXIST;
    thread = start_driver(&driver, NULL);
    ret = GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.item, "expected the item dialog\n");
    ok(!ret, "expected FALSE on cancel\n");
    ok(CommDlgExtendedError() == 0, "error %#lx\n", CommDlgExtendedError());
}

static UINT_PTR CALLBACK hook_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return 0;
}

static void test_hook_keeps_classic(const WCHAR *dir)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;

    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    ofn.Flags |= OFN_ENABLEHOOK;
    ofn.lpfnHook = hook_proc;
    thread = start_driver(&driver, NULL);
    GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.found, "dialog not found\n");
    ok(!driver.item, "a hook must keep the classic dialog\n");
}

static void test_kill_switch(const WCHAR *dir)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    HKEY key;
    DWORD one = 1;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, PolicyKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
    {
        skip("cannot write the policy key\n");
        return;
    }
    RegSetValueExW(key, L"UseClassicFileDialog", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    thread = start_driver(&driver, NULL);
    GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.found, "dialog not found\n");
    ok(!driver.item, "UseClassicFileDialog must keep the classic dialog\n");
    RegDeleteValueW(key, L"UseClassicFileDialog");
    RegCloseKey(key);
}

static void test_buffer_too_small(const WCHAR *dir)
{
    WCHAR file[4] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    BOOL ret;

    create_file(dir, L"routesmall.txt");
    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    thread = start_driver(&driver, L"routesmall.txt");
    ret = GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.item, "expected the item dialog\n");
    ok(!ret, "expected FALSE\n");
    ok(CommDlgExtendedError() == FNERR_BUFFERTOOSMALL, "error %#lx\n", CommDlgExtendedError());
    ok(*(WORD *)file > ARRAY_SIZE(file), "needed size %u\n", *(WORD *)file);
    delete_file(dir, L"routesmall.txt");
}

static void test_multiselect(const WCHAR *dir)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    const WCHAR *p;
    BOOL ret;

    create_file(dir, L"routea.txt");
    create_file(dir, L"routeb.txt");
    init_ofn(&ofn, file, ARRAY_SIZE(file), dir);
    ofn.Flags |= OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST;
    thread = start_driver(&driver, L"\"routea.txt\" \"routeb.txt\"");
    ret = GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.item, "expected the item dialog\n");
    ok(ret, "GetOpenFileNameW failed, error %#lx\n", CommDlgExtendedError());
    if (ret)
    {
        ok(!lstrcmpiW(file, dir), "directory %s\n", wine_dbgstr_w(file));
        ok(ofn.nFileOffset == lstrlenW(dir) + 1, "nFileOffset %u\n", ofn.nFileOffset);
        p = file + ofn.nFileOffset;
        ok(!lstrcmpiW(p, L"routea.txt") || !lstrcmpiW(p, L"routeb.txt"), "first %s\n", wine_dbgstr_w(p));
        p += lstrlenW(p) + 1;
        ok(!lstrcmpiW(p, L"routea.txt") || !lstrcmpiW(p, L"routeb.txt"), "second %s\n", wine_dbgstr_w(p));
        p += lstrlenW(p) + 1;
        ok(!*p, "list not double-null terminated\n");
    }
    delete_file(dir, L"routea.txt");
    delete_file(dir, L"routeb.txt");
}

static void test_ansi(const WCHAR *dir)
{
    char file[MAX_PATH] = "", dirA[MAX_PATH], expected[MAX_PATH];
    OPENFILENAMEA ofn;
    struct driver driver;
    HANDLE thread;
    BOOL ret;

    WideCharToMultiByte(CP_ACP, 0, dir, -1, dirA, sizeof(dirA), NULL, NULL);
    create_file(dir, L"routeansi.txt");
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = ROUTE_TITLE_A;
    ofn.lpstrFilter = "Text files\0*.txt\0";
    ofn.lpstrInitialDir = dirA;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    thread = start_driver(&driver, L"routeansi.txt");
    ret = GetOpenFileNameA(&ofn);
    finish_driver(thread);
    ok(driver.item, "expected the item dialog\n");
    ok(ret, "GetOpenFileNameA failed, error %#lx\n", CommDlgExtendedError());
    lstrcpyA(expected, dirA);
    lstrcatA(expected, "\\routeansi.txt");
    ok(!lstrcmpiA(file, expected), "got %s, expected %s\n", file, expected);
    ok(ofn.nFileOffset == lstrlenA(dirA) + 1, "nFileOffset %u\n", ofn.nFileOffset);
    delete_file(dir, L"routeansi.txt");
}

/* No file buffer: the ANSI path must not touch it and keeps the classic dialog. */
static void test_ansi_null_file(void)
{
    OPENFILENAMEA ofn;
    struct driver driver;
    HANDLE thread;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = ROUTE_TITLE_A;
    ofn.Flags = OFN_EXPLORER;
    thread = start_driver(&driver, NULL);
    GetOpenFileNameA(&ofn);
    finish_driver(thread);
    ok(!driver.found || !driver.item, "expected no item dialog without a file buffer\n");
}

/* A caller in a multithreaded apartment still gets a working dialog. */
static DWORD WINAPI mta_proc(void *arg)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    struct driver driver;
    HANDLE thread;
    BOOL ret;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    init_ofn(&ofn, file, ARRAY_SIZE(file), arg);
    thread = start_driver(&driver, NULL);
    ret = GetOpenFileNameW(&ofn);
    finish_driver(thread);
    ok(driver.found, "dialog not found in an MTA thread\n");
    ok(!ret, "expected FALSE on cancel\n");
    ok(CommDlgExtendedError() == 0, "error %#lx\n", CommDlgExtendedError());
    CoUninitialize();
    return 0;
}

static void test_mta(WCHAR *dir)
{
    HANDLE thread = CreateThread(NULL, 0, mta_proc, dir, 0, NULL);

    ok(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0, "MTA thread did not finish\n");
    CloseHandle(thread);
}

START_TEST(itemroute)
{
    WCHAR dir[MAX_PATH];

    get_test_dir(dir);
    test_save_ok(dir);
    test_open_cancel(dir);
    test_hook_keeps_classic(dir);
    test_kill_switch(dir);
    test_buffer_too_small(dir);
    test_multiselect(dir);
    test_ansi(dir);
    test_ansi_null_file();
    test_mta(dir);
    delete_file(dir, L"routesave.txt");
    delete_file(dir, L"routesave2.txt");
    RemoveDirectoryW(dir);
}
