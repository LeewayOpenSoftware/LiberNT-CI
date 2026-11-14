/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Screen saver enumeration and settings (SystemParametersInfo + SCRNSAVE.EXE)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>

static const WCHAR DesktopKey[] = L"Control Panel\\Desktop";

BOOL
LSaverIsAvailable(void)
{
    BOOL active;

    return SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &active, 0);
}

int
LSaverEnum(LSAVER_ITEM *out, int max)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int count = 0, i, j;

    if (max <= 0 || !GetSystemDirectoryW(dir, ARRAYSIZE(dir)))
        return 0;
    lstrcpyW(pattern, dir);
    lstrcatW(pattern, L"\\*.scr");
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        WCHAR *dot;

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            lstrlenW(dir) + 1 + lstrlenW(data.cFileName) >= MAX_PATH)
            continue;
        lstrcpyW(out[count].Path, dir);
        lstrcatW(out[count].Path, L"\\");
        lstrcatW(out[count].Path, data.cFileName);
        lstrcpynW(out[count].Name, data.cFileName, ARRAYSIZE(out[count].Name));
        dot = wcsrchr(out[count].Name, L'.');
        if (dot)
            *dot = 0;
        count++;
    } while (count < max && FindNextFileW(find, &data));
    FindClose(find);
    for (i = 1; i < count; i++)
    {
        LSAVER_ITEM key = out[i];

        for (j = i - 1; j >= 0 && lstrcmpiW(out[j].Name, key.Name) > 0; j--)
            out[j + 1] = out[j];
        out[j + 1] = key;
    }
    return count;
}

BOOL
LSaverGet(WCHAR *path, DWORD cch, UINT *timeoutSeconds, BOOL *active)
{
    HKEY key;
    DWORD type, size = cch * sizeof(WCHAR);
    UINT timeout = 0;
    BOOL on = FALSE;
    WCHAR raw[MAX_PATH];

    if (!SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT, 0, &timeout, 0) ||
        !SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &on, 0))
        return FALSE;
    path[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, DesktopKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        size = sizeof(raw) - sizeof(WCHAR);
        raw[0] = 0;
        if (RegQueryValueExW(key, L"SCRNSAVE.EXE", NULL, &type, (BYTE *)raw, &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            raw[size / sizeof(WCHAR)] = 0;
            if (type == REG_EXPAND_SZ)
            {
                if (!ExpandEnvironmentStringsW(raw, path, cch))
                    path[0] = 0;
            }
            else
                lstrcpynW(path, raw, cch);
        }
        RegCloseKey(key);
    }
    *timeoutSeconds = timeout;
    *active = on;
    return TRUE;
}

static BOOL
SetSaverPath(const WCHAR *path, DWORD type)
{
    HKEY key;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, DesktopKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);

    if (result != ERROR_SUCCESS)
        return FALSE;
    if (path && path[0])
        result = RegSetValueExW(key, L"SCRNSAVE.EXE", 0, type, (const BYTE *)path, (lstrlenW(path) + 1) * sizeof(WCHAR));
    else
    {
        result = RegDeleteValueW(key, L"SCRNSAVE.EXE");
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

BOOL
LSaverSet(const WCHAR *path, UINT timeoutSeconds, BOOL active)
{
    WCHAR oldPath[MAX_PATH], oldRaw[MAX_PATH];
    DWORD oldType = REG_SZ, oldSize = sizeof(oldRaw) - sizeof(WCHAR);
    BOOL hadOld = FALSE;
    HKEY key;
    UINT oldTimeout;
    BOOL oldActive;
    BOOL on = (path && path[0]) ? active : FALSE;

    if (timeoutSeconds < 60 || timeoutSeconds > 86400 * 7)
        return FALSE;
    if (!LSaverGet(oldPath, ARRAYSIZE(oldPath), &oldTimeout, &oldActive))
        return FALSE;
    oldRaw[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, DesktopKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        hadOld = RegQueryValueExW(key, L"SCRNSAVE.EXE", NULL, &oldType, (BYTE *)oldRaw, &oldSize) == ERROR_SUCCESS &&
                 (oldType == REG_SZ || oldType == REG_EXPAND_SZ);
        if (hadOld)
            oldRaw[oldSize / sizeof(WCHAR)] = 0;
        RegCloseKey(key);
    }
    if (SetSaverPath(path, REG_SZ) &&
        SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, timeoutSeconds, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) &&
        SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, on, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return TRUE;
    /* Put all three values back so a failure never leaves a mixed configuration. */
    SetSaverPath(hadOld ? oldRaw : NULL, oldType);
    SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, oldTimeout, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, oldActive, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    return FALSE;
}

BOOL
LSaverTest(const WCHAR *path)
{
    WCHAR cmd[MAX_PATH + 16];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!path || !path[0] || lstrlenW(path) + 8 >= (int)ARRAYSIZE(cmd))
        return FALSE;
    lstrcpyW(cmd, L"\"");
    lstrcatW(cmd, path);
    lstrcatW(cmd, L"\" /s");
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}
