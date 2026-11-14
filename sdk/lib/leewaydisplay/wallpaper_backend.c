/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Wallpaper enumeration and apply (SystemParametersInfo + WallpaperStyle registry)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>

static const WCHAR DesktopKey[] = L"Control Panel\\Desktop";

static const struct { LPCWSTR Style; LPCWSTR Tile; } StyleValues[LWALL_STYLE_COUNT] =
{
    { L"10", L"0" },  /* Fill */
    { L"6", L"0" },   /* Fit */
    { L"2", L"0" },   /* Stretch */
    { L"0", L"1" },   /* Tile */
    { L"0", L"0" },   /* Center */
};

static BOOL
IsImage(LPCWSTR name)
{
    LPCWSTR ext = wcsrchr(name, L'.');

    return ext && (!lstrcmpiW(ext, L".bmp") || !lstrcmpiW(ext, L".jpg") || !lstrcmpiW(ext, L".jpeg") ||
                   !lstrcmpiW(ext, L".png"));
}

int
LWallEnum(LWALL_ITEM *out, int max)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int count = 0, i, j;

    if (max <= 0 || !GetWindowsDirectoryW(dir, ARRAYSIZE(dir)))
        return 0;
    lstrcatW(dir, L"\\Web\\Wallpaper");
    lstrcpyW(pattern, dir);
    lstrcatW(pattern, L"\\*");
    find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        WCHAR *dot;

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImage(data.cFileName))
            continue;
        if (lstrlenW(dir) + 1 + lstrlenW(data.cFileName) >= MAX_PATH)
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
        LWALL_ITEM key = out[i];

        for (j = i - 1; j >= 0 && lstrcmpiW(out[j].Name, key.Name) > 0; j--)
            out[j + 1] = out[j];
        out[j + 1] = key;
    }
    return count;
}

static BOOL
ReadString(LPCWSTR name, WCHAR *value, DWORD cch)
{
    HKEY key;
    DWORD type, size = cch * sizeof(WCHAR);
    BOOL ok;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, DesktopKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    ok = RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    return ok;
}

static BOOL
WriteString(LPCWSTR name, LPCWSTR value)
{
    HKEY key;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, DesktopKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);

    if (result != ERROR_SUCCESS)
        return FALSE;
    result = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

BOOL
LWallGetCurrent(WCHAR *path, DWORD cch, LWALL_STYLE *style)
{
    WCHAR s[16] = L"", t[16] = L"0";
    int i;

    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, cch, path, 0))
        return FALSE;
    ReadString(L"WallpaperStyle", s, ARRAYSIZE(s));
    ReadString(L"TileWallpaper", t, ARRAYSIZE(t));
    *style = LWALL_FILL;
    for (i = 0; i < LWALL_STYLE_COUNT; i++)
    {
        if (!lstrcmpW(s, StyleValues[i].Style) && !lstrcmpW(t, StyleValues[i].Tile))
        {
            *style = (LWALL_STYLE)i;
            break;
        }
    }
    return TRUE;
}

BOOL
LWallSet(const WCHAR *path, LWALL_STYLE style)
{
    WCHAR oldStyle[16] = L"", oldTile[16] = L"";
    BOOL hadStyle, hadTile;

    if ((int)style < 0 || style >= LWALL_STYLE_COUNT)
        return FALSE;
    hadStyle = ReadString(L"WallpaperStyle", oldStyle, ARRAYSIZE(oldStyle));
    hadTile = ReadString(L"TileWallpaper", oldTile, ARRAYSIZE(oldTile));
    if (!WriteString(L"WallpaperStyle", StyleValues[style].Style) || !WriteString(L"TileWallpaper", StyleValues[style].Tile))
        return FALSE;
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return TRUE;
    /* Keep the old picture and the old position together when the new image is refused. */
    if (hadStyle)
        WriteString(L"WallpaperStyle", oldStyle);
    if (hadTile)
        WriteString(L"TileWallpaper", oldTile);
    return FALSE;
}
