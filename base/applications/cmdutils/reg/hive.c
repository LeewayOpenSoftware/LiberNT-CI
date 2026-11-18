/*
 * PROJECT:     LiberNT reg.exe
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     REG SAVE, REG RESTORE, REG LOAD and REG UNLOAD
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "reg.h"

static void enable_privilege(const WCHAR *name)
{
    TOKEN_PRIVILEGES tp;
    HANDLE token;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);

    CloseHandle(token);
}

static int output_error(LONG error)
{
    WCHAR *str;

    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    {
        output_message(STRING_REG_NOT_FOUND);
        return 1;
    }

    if (!FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, (WCHAR *)&str, 0, NULL))
    {
        output_string(L"ERROR: %1!u!\n", error);
        return 1;
    }

    output_string(L"ERROR: %1", str);
    LocalFree(str);
    return 1;
}

static BOOL parse_view_switch(const WCHAR *arg, REGSAM *sam)
{
    if (arg[0] != '/' && arg[0] != '-')
        return FALSE;

    if (!lstrcmpiW(arg + 1, L"reg:32"))
    {
        if (*sam & KEY_WOW64_32KEY) return FALSE;
        *sam |= KEY_WOW64_32KEY;
    }
    else if (!lstrcmpiW(arg + 1, L"reg:64"))
    {
        if (*sam & KEY_WOW64_64KEY) return FALSE;
        *sam |= KEY_WOW64_64KEY;
    }
    else
        return FALSE;

    return *sam != (KEY_WOW64_32KEY | KEY_WOW64_64KEY);
}

static int invalid_syntax(WCHAR *op)
{
    output_message(STRING_INVALID_SYNTAX);
    output_message(STRING_FUNC_HELP, wcsupr(op));
    return 1;
}

int reg_save(int argc, WCHAR *argvW[])
{
    BOOL overwrite_file = FALSE;
    REGSAM sam = 0;
    HKEY root, hkey;
    WCHAR *path;
    LONG error;
    int i;

    if (argc < 4)
        return invalid_syntax(argvW[1]);

    for (i = 4; i < argc; i++)
    {
        if ((argvW[i][0] == '/' || argvW[i][0] == '-') && is_char(argvW[i][1], 'y') && !argvW[i][2])
        {
            if (overwrite_file) return invalid_syntax(argvW[1]);
            overwrite_file = TRUE;
        }
        else if (!parse_view_switch(argvW[i], &sam))
            return invalid_syntax(argvW[1]);
    }

    if (!parse_registry_key(argvW[2], &root, &path))
        return 1;

    error = RegOpenKeyExW(root, path, 0, KEY_READ | sam, &hkey);
    if (error)
        return output_error(error);

    if (GetFileAttributesW(argvW[3]) != INVALID_FILE_ATTRIBUTES)
    {
        if (!overwrite_file && !ask_confirm(STRING_OVERWRITE_FILE, argvW[3]))
        {
            RegCloseKey(hkey);
            output_message(STRING_CANCELLED);
            return 0;
        }

        if (!DeleteFileW(argvW[3]))
        {
            error = GetLastError();
            RegCloseKey(hkey);
            return output_error(error);
        }
    }

    enable_privilege(SE_BACKUP_NAME);
    error = RegSaveKeyExW(hkey, argvW[3], NULL, REG_LATEST_FORMAT);
    RegCloseKey(hkey);
    if (error)
        return output_error(error);

    output_message(STRING_SUCCESS);
    return 0;
}

int reg_restore(int argc, WCHAR *argvW[])
{
    REGSAM sam = 0;
    HKEY root, hkey;
    WCHAR *path;
    LONG error;
    int i;

    if (argc < 4)
        return invalid_syntax(argvW[1]);

    for (i = 4; i < argc; i++)
    {
        if (!parse_view_switch(argvW[i], &sam))
            return invalid_syntax(argvW[1]);
    }

    if (!parse_registry_key(argvW[2], &root, &path))
        return 1;

    error = RegOpenKeyExW(root, path, 0, KEY_ALL_ACCESS | sam, &hkey);
    if (error)
        return output_error(error);

    if (GetFileAttributesW(argvW[3]) == INVALID_FILE_ATTRIBUTES)
    {
        RegCloseKey(hkey);
        return output_error(GetLastError());
    }

    enable_privilege(SE_BACKUP_NAME);
    enable_privilege(SE_RESTORE_NAME);
    error = RegRestoreKeyW(hkey, argvW[3], REG_FORCE_RESTORE);
    RegCloseKey(hkey);
    if (error)
        return output_error(error);

    output_message(STRING_SUCCESS);
    return 0;
}

/* LOAD and UNLOAD take a subkey of HKLM or HKU */
static BOOL parse_hive_key(WCHAR *op, const WCHAR *key, HKEY *root, WCHAR **path)
{
    if (!parse_registry_key(key, root, path))
        return FALSE;

    if (*root != HKEY_LOCAL_MACHINE && *root != HKEY_USERS)
    {
        output_message(STRING_HIVE_ROOT);
        output_message(STRING_FUNC_HELP, wcsupr(op));
        return FALSE;
    }

    if (!*path)
    {
        invalid_syntax(op);
        return FALSE;
    }

    return TRUE;
}

int reg_load(int argc, WCHAR *argvW[])
{
    REGSAM sam = 0;
    HKEY root;
    WCHAR *path;
    LONG error;
    int i;

    if (argc < 4)
        return invalid_syntax(argvW[1]);

    for (i = 4; i < argc; i++)
    {
        if (!parse_view_switch(argvW[i], &sam))
            return invalid_syntax(argvW[1]);
    }

    if (!parse_hive_key(argvW[1], argvW[2], &root, &path))
        return 1;

    enable_privilege(SE_BACKUP_NAME);
    enable_privilege(SE_RESTORE_NAME);
    error = RegLoadKeyW(root, path, argvW[3]);
    if (error)
        return output_error(error);

    output_message(STRING_SUCCESS);
    return 0;
}

int reg_unload(int argc, WCHAR *argvW[])
{
    HKEY root;
    WCHAR *path;
    LONG error;

    if (argc != 3)
        return invalid_syntax(argvW[1]);

    if (!parse_hive_key(argvW[1], argvW[2], &root, &path))
        return 1;

    enable_privilege(SE_BACKUP_NAME);
    enable_privilege(SE_RESTORE_NAME);
    error = RegUnLoadKeyW(root, path);
    if (error)
        return output_error(error);

    output_message(STRING_SUCCESS);
    return 0;
}
