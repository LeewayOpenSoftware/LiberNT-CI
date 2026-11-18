/*
 * PROJECT:     LiberNT reg.exe tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     REG SAVE, RESTORE, LOAD, UNLOAD and COMPARE
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "reg_test.h"

#define CMP1 KEY_WINE "\\reg_cmp1"
#define CMP2 KEY_WINE "\\reg_cmp2"
#define SAVE_KEY KEY_WINE "\\reg_save"
#define RESTORE_KEY KEY_WINE "\\reg_restore"
#define HIVE_FILE "reg_test.hiv"

static void test_syntax(void)
{
    static const char *cmds[] =
    {
        "reg save", "reg save HKCU\\" SAVE_KEY, "reg save HKCU\\" SAVE_KEY " f.hiv /y /y",
        "reg save HKCU\\" SAVE_KEY " f.hiv /reg:32 /reg:64",
        "reg restore", "reg restore HKCU\\" SAVE_KEY,
        "reg load", "reg load HKLM\\reg_test_hive", "reg load HKLM f.hiv", "reg load HKCU\\reg_test_hive f.hiv",
        "reg unload", "reg unload HKLM", "reg unload HKLM\\reg_test_hive x", "reg unload HKCU\\reg_test_hive",
        "reg compare", "reg compare HKCU\\" CMP1, "reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /v",
        "reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /v a /ve", "reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /oa /od",
        "reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /s /s", "reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /x",
    };
    static const char *help[] = { "reg save /?", "reg restore /?", "reg load /?", "reg unload /?", "reg compare /?" };
    DWORD r, i;

    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        run_reg_exe(cmds[i], &r);
        ok(r == REG_EXIT_FAILURE, "%s: got exit code %ld, expected 1\n", cmds[i], r);
    }

    for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
    {
        run_reg_exe(help[i], &r);
        ok(r == REG_EXIT_SUCCESS, "%s: got exit code %ld, expected 0\n", help[i], r);
    }
}

static void test_compare(void)
{
    HKEY k1, k2, sub;
    DWORD r, dword = 1;

    delete_tree(HKEY_CURRENT_USER, CMP1, 0);
    delete_tree(HKEY_CURRENT_USER, CMP2, 0);
    add_key(HKEY_CURRENT_USER, CMP1, 0, &k1);
    add_key(HKEY_CURRENT_USER, CMP2, 0, &k2);
    add_value(k1, "same", REG_SZ, "data", 5);
    add_value(k2, "same", REG_SZ, "data", 5);
    add_value(k1, "num", REG_DWORD, &dword, sizeof(dword));
    add_value(k2, "num", REG_DWORD, &dword, sizeof(dword));

    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2, &r);
    ok(r == 0, "identical: got exit code %ld, expected 0\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /oa", &r);
    ok(r == 0, "identical /oa: got exit code %ld, expected 0\n", r);

    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP1, &r);
    ok(r == REG_EXIT_FAILURE, "self: got exit code %ld, expected 1\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " \\\\.", &r);
    ok(r == REG_EXIT_FAILURE, "self \\\\.: got exit code %ld, expected 1\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" KEY_WINE "\\reg_nonexist", &r);
    ok(r == REG_EXIT_FAILURE, "missing key: got exit code %ld, expected 1\n", r);

    dword = 2;
    add_value(k2, "num", REG_DWORD, &dword, sizeof(dword));
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2, &r);
    ok(r == 2, "different: got exit code %ld, expected 2\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /v same", &r);
    ok(r == 0, "/v same: got exit code %ld, expected 0\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /v num /on", &r);
    ok(r == 2, "/v num: got exit code %ld, expected 2\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /v nothere", &r);
    ok(r == REG_EXIT_FAILURE, "/v missing: got exit code %ld, expected 1\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /ve", &r);
    ok(r == REG_EXIT_FAILURE, "/ve missing: got exit code %ld, expected 1\n", r);

    dword = 1;
    add_value(k2, "num", REG_DWORD, &dword, sizeof(dword));
    add_key(k1, "child", 0, &sub);
    close_key(sub);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2, &r);
    ok(r == 0, "subkey without /s: got exit code %ld, expected 0\n", r);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /s", &r);
    ok(r == 2, "subkey with /s: got exit code %ld, expected 2\n", r);

    add_key(k2, "child", 0, &sub);
    close_key(sub);
    run_reg_exe("reg compare HKCU\\" CMP1 " HKCU\\" CMP2 " /s /os", &r);
    ok(r == 0, "same subkeys /s: got exit code %ld, expected 0\n", r);

    close_key(k1);
    close_key(k2);
    delete_tree(HKEY_CURRENT_USER, CMP1, 0);
    delete_tree(HKEY_CURRENT_USER, CMP2, 0);
}

static void test_save_load(void)
{
    char path[MAX_PATH], cmd[MAX_PATH * 2];
    BYTE header[0x20];
    HANDLE file;
    HKEY hkey;
    DWORD r, read;

    delete_tree(HKEY_CURRENT_USER, SAVE_KEY, 0);
    add_key(HKEY_CURRENT_USER, SAVE_KEY, 0, &hkey);
    add_value(hkey, "saved", REG_SZ, "yes", 4);
    close_key(hkey);

    run_reg_exe("reg save HKCU\\" KEY_WINE "\\reg_nonexist " HIVE_FILE, &r);
    ok(r == REG_EXIT_FAILURE, "save missing key: got exit code %ld, expected 1\n", r);

    if (!is_elevated_process())
    {
        win_skip("not elevated: REG SAVE and LOAD need the backup and restore privileges\n");
        delete_tree(HKEY_CURRENT_USER, SAVE_KEY, 0);
        return;
    }

    GetFullPathNameA(HIVE_FILE, sizeof(path), path, NULL);
    DeleteFileA(path);

    sprintf(cmd, "reg save HKCU\\" SAVE_KEY " \"%s\"", path);
    run_reg_exe(cmd, &r);
    ok(r == REG_EXIT_SUCCESS, "save: got exit code %ld, expected 0\n", r);

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(file != INVALID_HANDLE_VALUE, "hive file missing, error %lu\n", GetLastError());
    if (file != INVALID_HANDLE_VALUE)
    {
        ok(ReadFile(file, header, sizeof(header), &read, NULL) && read == sizeof(header), "read failed\n");
        ok(!memcmp(header, "regf", 4), "no regf signature\n");
        ok(*(DWORD *)&header[0x14] == 1 && *(DWORD *)&header[0x18] == 5,
           "hive format %lu.%lu, expected 1.5\n", *(DWORD *)&header[0x14], *(DWORD *)&header[0x18]);
        CloseHandle(file);
    }

    sprintf(cmd, "reg save HKCU\\" SAVE_KEY " \"%s\" /y", path);
    run_reg_exe(cmd, &r);
    ok(r == REG_EXIT_SUCCESS, "save /y over existing: got exit code %ld, expected 0\n", r);

    sprintf(cmd, "reg load HKLM\\reg_test_hive \"%s\"", path);
    run_reg_exe(cmd, &r);
    ok(r == REG_EXIT_SUCCESS, "load: got exit code %ld, expected 0\n", r);
    if (r == REG_EXIT_SUCCESS)
    {
        open_key(HKEY_LOCAL_MACHINE, "reg_test_hive", 0, &hkey);
        verify_reg(hkey, "saved", REG_SZ, "yes", 4, 0);
        close_key(hkey);

        run_reg_exe("reg compare HKCU\\" SAVE_KEY " HKLM\\reg_test_hive", &r);
        ok(r == 0, "compare saved and loaded: got exit code %ld, expected 0\n", r);

        run_reg_exe("reg unload HKLM\\reg_test_hive", &r);
        ok(r == REG_EXIT_SUCCESS, "unload: got exit code %ld, expected 0\n", r);
        verify_key_nonexist(HKEY_LOCAL_MACHINE, "reg_test_hive", 0);
    }

    run_reg_exe("reg unload HKLM\\reg_test_hive", &r);
    ok(r == REG_EXIT_FAILURE, "unload twice: got exit code %ld, expected 1\n", r);

    delete_tree(HKEY_CURRENT_USER, RESTORE_KEY, 0);
    add_key(HKEY_CURRENT_USER, RESTORE_KEY, 0, &hkey);
    add_value(hkey, "stale", REG_SZ, "old", 4);
    close_key(hkey);
    sprintf(cmd, "reg restore HKCU\\" RESTORE_KEY " \"%s\"", path);
    run_reg_exe(cmd, &r);
    ok(r == REG_EXIT_SUCCESS, "restore: got exit code %ld, expected 0\n", r);
    run_reg_exe("reg compare HKCU\\" SAVE_KEY " HKCU\\" RESTORE_KEY, &r);
    ok(r == 0, "compare saved and restored: got exit code %ld, expected 0\n", r);
    open_key(HKEY_CURRENT_USER, RESTORE_KEY, 0, &hkey);
    verify_reg_nonexist(hkey, "stale");
    close_key(hkey);
    delete_tree(HKEY_CURRENT_USER, RESTORE_KEY, 0);

    sprintf(cmd, "reg restore HKCU\\" KEY_WINE "\\reg_nonexist \"%s\"", path);
    run_reg_exe(cmd, &r);
    ok(r == REG_EXIT_FAILURE, "restore missing key: got exit code %ld, expected 1\n", r);

    DeleteFileA(path);
    delete_tree(HKEY_CURRENT_USER, SAVE_KEY, 0);
}

START_TEST(hive)
{
    DWORD r;

    if (!run_reg_exe("reg.exe /?", &r))
    {
        win_skip("reg.exe not available, skipping 'hive' tests\n");
        return;
    }

    test_syntax();
    test_compare();
    test_save_load();
}
