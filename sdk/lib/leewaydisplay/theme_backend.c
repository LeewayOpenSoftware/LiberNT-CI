/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Theme enumeration, thumbnail composite, and theme application
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>
#include <leewayui/theme_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static const WCHAR UnsavedThemeName[] = L"Unsaved Theme";

/* Read an INI string value from a .theme file */
static BOOL
ReadThemeString(const WCHAR *theme_path, const WCHAR *section, const WCHAR *key,
                WCHAR *out_value, DWORD out_cch)
{
    return GetPrivateProfileStringW(section, key, L"", out_value, out_cch, theme_path) > 0;
}

/* Get the display name from a .theme file */
static BOOL
GetThemeDisplayName(const WCHAR *theme_path, WCHAR *out_name, DWORD out_cch)
{
    WCHAR name[128];
    if (!ReadThemeString(theme_path, L"Theme", L"DisplayName", name, ARRAYSIZE(name)))
        return FALSE;
    lstrcpynW(out_name, name, out_cch);
    return TRUE;
}

/* Get wallpaper path from .theme file */
static BOOL
GetThemeWallpaper(const WCHAR *theme_path, WCHAR *out_path, DWORD out_cch, LWALL_STYLE *out_style)
{
    WCHAR wallpaper[MAX_PATH];
    WCHAR style_str[16];
    WCHAR tile_str[16];

    if (!ReadThemeString(theme_path, L"Control Panel\\Desktop", L"Wallpaper",
                         wallpaper, ARRAYSIZE(wallpaper)))
        return FALSE;

    if (wcslen(wallpaper) == 0)
        return FALSE;

    lstrcpynW(out_path, wallpaper, out_cch);

    ReadThemeString(theme_path, L"Control Panel\\Desktop", L"WallpaperStyle", style_str, ARRAYSIZE(style_str));
    ReadThemeString(theme_path, L"Control Panel\\Desktop", L"TileWallpaper", tile_str, ARRAYSIZE(tile_str));

    int style = 0;
    int tile = 0;
    if (wcslen(style_str) > 0) style = _wtoi(style_str);
    if (wcslen(tile_str) > 0) tile = _wtoi(tile_str);

    if (tile)
        *out_style = LWALL_TILE;
    else if (style == 10)
        *out_style = LWALL_FILL;
    else if (style == 6)
        *out_style = LWALL_FIT;
    else if (style == 2)
        *out_style = LWALL_STRETCH;
    else
        *out_style = LWALL_CENTER;

    return TRUE;
}

/* Get window color from theme */
static BOOL
GetThemeWindowColor(const WCHAR *theme_path, COLORREF *out_color)
{
    WCHAR color_str[32];
    if (ReadThemeString(theme_path, L"Control Panel\\Desktop", L"ActiveTitle", color_str, ARRAYSIZE(color_str)))
    {
        if (wcslen(color_str) > 0)
        {
            int r, g, b;
            if (swscanf(color_str, L"%d %d %d", &r, &g, &b) == 3)
            {
                *out_color = RGB(r, g, b);
                return TRUE;
            }
        }
    }

    *out_color = RGB(0, 102, 204);
    return TRUE;
}

/* Get screensaver from theme */
static BOOL
GetThemeScreensaver(const WCHAR *theme_path, WCHAR *out_path, DWORD out_cch, UINT *out_timeout, BOOL *out_active)
{
    WCHAR saver_path[MAX_PATH];
    WCHAR timeout_str[16];
    WCHAR active_str[16];

    if (!ReadThemeString(theme_path, L"Screen Saver", L"SCRNSAVE.EXE", saver_path, ARRAYSIZE(saver_path)))
        return FALSE;

    if (wcslen(saver_path) == 0)
        return FALSE;

    lstrcpynW(out_path, saver_path, out_cch);

    if (ReadThemeString(theme_path, L"Screen Saver", L"ScreenSaveTimeOut", timeout_str, ARRAYSIZE(timeout_str)))
        *out_timeout = _wtoi(timeout_str);
    else
        *out_timeout = 600;

    if (ReadThemeString(theme_path, L"Screen Saver", L"ScreenSaveActive", active_str, ARRAYSIZE(active_str)))
        *out_active = (_wtoi(active_str) != 0);
    else
        *out_active = FALSE;

    return TRUE;
}

/* Create thumbnail: wallpaper preview (180x135) + color swatch (40x40) */
HBITMAP WINAPI
LThemeCreateThumbnail(const WCHAR *wallpaper_path, COLORREF window_color)
{
    const int THUMB_WIDTH = 200;
    const int THUMB_HEIGHT = 150;
    const int WALL_WIDTH = 180;
    const int WALL_HEIGHT = 135;
    const int SWATCH_SIZE = 40;

    HDC hdc = GetDC(NULL);
    if (!hdc) return NULL;

    HDC hdc_mem = CreateCompatibleDC(hdc);
    if (!hdc_mem)
    {
        ReleaseDC(NULL, hdc);
        return NULL;
    }

    HBITMAP hbm = CreateCompatibleBitmap(hdc, THUMB_WIDTH, THUMB_HEIGHT);
    if (!hbm)
    {
        DeleteDC(hdc_mem);
        ReleaseDC(NULL, hdc);
        return NULL;
    }

    HBITMAP old_bm = (HBITMAP)SelectObject(hdc_mem, hbm);

    HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
    RECT rc_bg = {0, 0, THUMB_WIDTH, THUMB_HEIGHT};
    FillRect(hdc_mem, &rc_bg, white_brush);
    DeleteObject(white_brush);

    if (wallpaper_path && wcslen(wallpaper_path) > 0)
    {
        HBITMAP hbm_wall = (HBITMAP)LoadImageW(NULL, wallpaper_path, IMAGE_BITMAP, 0, 0,
                                               LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (hbm_wall)
        {
            BITMAP bmp_orig;
            if (GetObjectW(hbm_wall, sizeof(bmp_orig), &bmp_orig))
            {
                float aspect = (float)bmp_orig.bmWidth / bmp_orig.bmHeight;
                int scaled_w = WALL_WIDTH;
                int scaled_h = (int)(WALL_WIDTH / aspect);

                if (scaled_h > WALL_HEIGHT)
                {
                    scaled_h = WALL_HEIGHT;
                    scaled_w = (int)(WALL_HEIGHT * aspect);
                }

                HDC hdc_wall = CreateCompatibleDC(hdc_mem);
                HBITMAP old_wall = (HBITMAP)SelectObject(hdc_wall, hbm_wall);

                HDC hdc_scaled = CreateCompatibleDC(hdc_mem);
                HBITMAP hbm_scaled = CreateCompatibleBitmap(hdc_mem, scaled_w, scaled_h);
                HBITMAP old_scaled = (HBITMAP)SelectObject(hdc_scaled, hbm_scaled);

                StretchBlt(hdc_scaled, 0, 0, scaled_w, scaled_h,
                          hdc_wall, 0, 0, bmp_orig.bmWidth, bmp_orig.bmHeight, SRCCOPY);

                int x_offset = (WALL_WIDTH - scaled_w) / 2;
                int y_offset = (WALL_HEIGHT - scaled_h) / 2;

                BitBlt(hdc_mem, x_offset, y_offset, scaled_w, scaled_h,
                       hdc_scaled, 0, 0, SRCCOPY);

                SelectObject(hdc_scaled, old_scaled);
                SelectObject(hdc_wall, old_wall);
                DeleteDC(hdc_scaled);
                DeleteDC(hdc_wall);
                DeleteObject(hbm_scaled);
            }
            DeleteObject(hbm_wall);
        }
    }

    HBRUSH color_brush = CreateSolidBrush(window_color);
    RECT rc_swatch = {THUMB_WIDTH - SWATCH_SIZE, THUMB_HEIGHT - SWATCH_SIZE,
                      THUMB_WIDTH, THUMB_HEIGHT};
    FillRect(hdc_mem, &rc_swatch, color_brush);
    DeleteObject(color_brush);

    SelectObject(hdc_mem, old_bm);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc);

    return hbm;
}

    HBITMAP hbm = CreateCompatibleBitmap(hdc, THUMB_WIDTH, THUMB_HEIGHT);
    if (!hbm)
    {
        DeleteDC(hdc_mem);
        ReleaseDC(NULL, hdc);
        return NULL;
    }

    HBITMAP old_bm = (HBITMAP)SelectObject(hdc_mem, hbm);

    HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
    RECT rc_bg = {0, 0, THUMB_WIDTH, THUMB_HEIGHT};
    FillRect(hdc_mem, &rc_bg, white_brush);
    DeleteObject(white_brush);

    HBRUSH wall_brush = CreateSolidBrush(RGB(192, 192, 192));
    RECT rc_wall = {0, 0, WALL_WIDTH, WALL_HEIGHT};
    FillRect(hdc_mem, &rc_wall, wall_brush);
    DeleteObject(wall_brush);

    HBRUSH color_brush = CreateSolidBrush(window_color);
    RECT rc_swatch = {THUMB_WIDTH - SWATCH_SIZE, THUMB_HEIGHT - SWATCH_SIZE,
                      THUMB_WIDTH, THUMB_HEIGHT};
    FillRect(hdc_mem, &rc_swatch, color_brush);
    DeleteObject(color_brush);

    SelectObject(hdc_mem, old_bm);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc);

    return hbm;
}

/* Helper to expand environment variables */
static BOOL
ExpandThemePath(const WCHAR *template_path, WCHAR *out_expanded, DWORD out_cch)
{
    DWORD expanded = ExpandEnvironmentStringsW(template_path, out_expanded, out_cch);
    return expanded > 0 && expanded <= out_cch;
}

/* Scan a directory for *.theme files and add to list */
static UINT
ScanThemesInDirectory(const WCHAR *dir_path, LTHEME_ITEM *items, UINT max_items, UINT start_idx)
{
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    WCHAR pattern[MAX_PATH];
    UINT count = start_idx;

    if (!dir_path || wcslen(dir_path) == 0)
        return count;

    lstrcpyW(pattern, dir_path);
    if (pattern[wcslen(pattern) - 1] != L'\\')
        lstrcatW(pattern, L"\\");
    lstrcatW(pattern, L"*.theme");

    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return count;

    do
    {
        WCHAR theme_path[MAX_PATH];
        WCHAR theme_name[128];

        if (count >= max_items)
            break;

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        lstrcpyW(theme_path, dir_path);
        if (theme_path[wcslen(theme_path) - 1] != L'\\')
            lstrcatW(theme_path, L"\\");
        lstrcatW(theme_path, find_data.cFileName);

        if (!GetThemeDisplayName(theme_path, theme_name, ARRAYSIZE(theme_name)))
        {
            lstrcpyW(theme_name, find_data.cFileName);
            WCHAR *dot = wcsrchr(theme_name, L'.');
            if (dot) *dot = L'\0';
        }

        LTHEME_ITEM *item = &items[count];
        lstrcpyW(item->Name, theme_name);
        lstrcpyW(item->ThemePath, theme_path);
        ZeroMemory(&item->Wallpaper, sizeof(item->Wallpaper));
        ZeroMemory(&item->Screensaver, sizeof(item->Screensaver));
        item->ScreensaverTimeout = 600;
        item->ScreensaverActive = FALSE;
        item->WindowColor = RGB(0, 102, 204);

        WCHAR wallpaper_path[MAX_PATH];
        if (GetThemeWallpaper(theme_path, wallpaper_path, MAX_PATH, &item->WallpaperStyle))
        {
            lstrcpyW(item->Wallpaper, wallpaper_path);
        }

        GetThemeWindowColor(theme_path, &item->WindowColor);

        UINT timeout;
        BOOL active;
        if (GetThemeScreensaver(theme_path, item->Screensaver, MAX_PATH, &timeout, &active))
        {
            item->ScreensaverTimeout = timeout;
            item->ScreensaverActive = active;
        }

        item->Thumbnail = LThemeCreateThumbnail(item->Wallpaper, item->WindowColor);

        count++;

    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
    return count;
}

/* Enumerate themes from both system and user directories */
HRESULT WINAPI
LThemeEnumerate(LTHEME_LIST *out)
{
    if (!out)
        return E_INVALIDARG;

    out->items = (LTHEME_ITEM *)malloc(LTHEME_MAX_ITEMS * sizeof(LTHEME_ITEM));
    if (!out->items)
        return E_OUTOFMEMORY;

    out->capacity = LTHEME_MAX_ITEMS;
    out->count = 0;

    LTHEME_ITEM *unsaved = &out->items[0];
    if (SUCCEEDED(LThemeGetUnsaved(unsaved)))
    {
        out->count++;
    }

    WCHAR system_themes_dir[MAX_PATH];
    WCHAR user_themes_dir[MAX_PATH];

    if (!ExpandThemePath(L"%WINDIR%\\Resources\\Themes", system_themes_dir, MAX_PATH))
        lstrcpyW(system_themes_dir, L"C:\\Windows\\Resources\\Themes");

    if (!ExpandThemePath(L"%APPDATA%\\Microsoft\\Windows\\Themes", user_themes_dir, MAX_PATH))
        lstrcpyW(user_themes_dir, L"");

    UINT count = ScanThemesInDirectory(system_themes_dir, out->items, out->capacity, out->count);
    out->count = count;

    if (user_themes_dir[0] != L'\0' && lstrcmpiW(user_themes_dir, system_themes_dir) != 0)
    {
        count = ScanThemesInDirectory(user_themes_dir, out->items, out->capacity, out->count);
        out->count = count;
    }

    return S_OK;
}

/* Free theme list */
void WINAPI
LThemeListFree(LTHEME_LIST *list)
{
    if (!list || !list->items)
        return;

    for (UINT i = 0; i < list->count; i++)
    {
        if (list->items[i].Thumbnail)
            DeleteObject(list->items[i].Thumbnail);
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Get current system settings as "Unsaved Theme" */
HRESULT WINAPI
LThemeGetUnsaved(LTHEME_ITEM *out)
{
    if (!out)
        return E_INVALIDARG;

    ZeroMemory(out, sizeof(*out));
    lstrcpyW(out->Name, UnsavedThemeName);
    out->ThemePath[0] = L'\0';

    BOOL ok = LWallGetCurrent(out->Wallpaper, MAX_PATH, &out->WallpaperStyle);
    if (!ok)
        return HRESULT_FROM_WIN32(GetLastError());

    out->WindowColor = RGB(0, 102, 204);
    out->Thumbnail = LThemeCreateThumbnail(out->Wallpaper, out->WindowColor);

    return S_OK;
}

/* Apply a theme */
HRESULT WINAPI
LThemeApply(const LTHEME_ITEM *theme)
{
    if (!theme)
        return E_INVALIDARG;

    if (wcslen(theme->Wallpaper) > 0)
    {
        if (!LWallSet(theme->Wallpaper, theme->WallpaperStyle))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    /* Apply window color */
    {
        WCHAR color_str[32];
        int r = GetRValue(theme->WindowColor);
        int g = GetGValue(theme->WindowColor);
        int b = GetBValue(theme->WindowColor);
        swprintf(color_str, ARRAYSIZE(color_str), L"%d %d %d", r, g, b);

        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\Colors", 0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, L"ActiveTitle", 0, REG_SZ, (PBYTE)color_str, (wcslen(color_str) + 1) * sizeof(WCHAR));
            RegCloseKey(key);
        }

        int colors[1] = {theme->WindowColor};
        int elements[1] = {COLOR_ACTIVECAPTION};
        SetSysColors(1, elements, colors);
    }

    /* Apply sound scheme */
    if (wcslen(theme->SoundsScheme) > 0)
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"AppEvents\Schemes", 0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, NULL, 0, REG_SZ, (PBYTE)theme->SoundsScheme, (wcslen(theme->SoundsScheme) + 1) * sizeof(WCHAR));
            RegCloseKey(key);
        }
    }


    if (wcslen(theme->Screensaver) > 0)
    {
        if (!LSaverSet(theme->Screensaver, theme->ScreensaverTimeout, theme->ScreensaverActive))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

/* Save current system settings as a new theme file */
HRESULT WINAPI
LThemeSave(const WCHAR *theme_name, WCHAR out_path[MAX_PATH])
{
    return E_NOTIMPL;
}
