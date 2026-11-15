/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Theme enumeration, thumbnail composite, and theme application
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>
#include <leewayui/theme_backend.h>
#include <stdlib.h>
#include <wchar.h>
#include <objbase.h>
#include <gdiplus.h>
#include <strsafe.h>

static const WCHAR UnsavedThemeName[] = L"Unsaved Theme";
static const WCHAR DesktopSection[] = L"Control Panel\\Desktop";
static const WCHAR ColorsSection[] = L"Control Panel\\Colors";
static const WCHAR SoundsSection[] = L"Sounds";
static const WCHAR BootSection[] = L"boot";
/* Read for tolerance only: earlier LiberNT theme files used it */
static const WCHAR LegacySaverSection[] = L"Screen Saver";
static const WCHAR SchemesKey[] = L"AppEvents\\Schemes";

#define THEME_DEFAULT_COLOR RGB(0, 102, 204)
#define THEME_DEFAULT_TIMEOUT 600

/* .theme reading **********************************************************/

static BOOL
ReadThemeString(const WCHAR *theme_path, const WCHAR *section, const WCHAR *key,
                WCHAR *out_value, DWORD out_cch)
{
    out_value[0] = L'\0';
    GetPrivateProfileStringW(section, key, L"", out_value, out_cch, theme_path);
    return out_value[0] != L'\0';
}

/* Read a path value and expand %SystemRoot% and the like */
static BOOL
ReadThemePath(const WCHAR *theme_path, const WCHAR *section, const WCHAR *key,
              WCHAR *out_path, DWORD out_cch)
{
    WCHAR raw[MAX_PATH];
    DWORD len;

    out_path[0] = L'\0';
    if (!ReadThemeString(theme_path, section, key, raw, ARRAYSIZE(raw)))
        return FALSE;

    len = ExpandEnvironmentStringsW(raw, out_path, out_cch);
    if (len == 0 || len > out_cch)
    {
        out_path[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

static BOOL
ReadThemeUInt(const WCHAR *theme_path, const WCHAR *section, const WCHAR *key, UINT *out_value)
{
    WCHAR value[16];

    if (!ReadThemeString(theme_path, section, key, value, ARRAYSIZE(value)))
        return FALSE;
    *out_value = (UINT)wcstoul(value, NULL, 10);
    return TRUE;
}

static void
ReadThemeWallpaper(const WCHAR *theme_path, LTHEME_ITEM *item)
{
    UINT style = 2, tile = 0;

    ReadThemePath(theme_path, DesktopSection, L"Wallpaper", item->Wallpaper, ARRAYSIZE(item->Wallpaper));
    ReadThemeUInt(theme_path, DesktopSection, L"WallpaperStyle", &style);
    ReadThemeUInt(theme_path, DesktopSection, L"TileWallpaper", &tile);

    if (tile)
        item->WallpaperStyle = LWALL_TILE;
    else if (style == 10)
        item->WallpaperStyle = LWALL_FILL;
    else if (style == 6)
        item->WallpaperStyle = LWALL_FIT;
    else if (style == 2)
        item->WallpaperStyle = LWALL_STRETCH;
    else
        item->WallpaperStyle = LWALL_CENTER;
}

static BOOL
ParseColor(const WCHAR *text, COLORREF *out_color)
{
    unsigned int r, g, b;

    if (swscanf(text, L"%u %u %u", &r, &g, &b) != 3 || r > 255 || g > 255 || b > 255)
        return FALSE;
    *out_color = RGB(r, g, b);
    return TRUE;
}

static void
ReadThemeWindowColor(const WCHAR *theme_path, LTHEME_ITEM *item)
{
    WCHAR value[32];

    item->WindowColor = THEME_DEFAULT_COLOR;
    if (ReadThemeString(theme_path, ColorsSection, L"ActiveTitle", value, ARRAYSIZE(value)))
        ParseColor(value, &item->WindowColor);
}

static void
ReadThemeScreensaver(const WCHAR *theme_path, LTHEME_ITEM *item)
{
    UINT timeout = THEME_DEFAULT_TIMEOUT, active = 0;

    /* Windows form: [boot] SCRNSAVE.EXE, the timing in [Control Panel\Desktop] */
    if (!ReadThemePath(theme_path, BootSection, L"SCRNSAVE.EXE", item->Screensaver, ARRAYSIZE(item->Screensaver)))
        ReadThemePath(theme_path, LegacySaverSection, L"SCRNSAVE.EXE", item->Screensaver, ARRAYSIZE(item->Screensaver));

    if (!ReadThemeUInt(theme_path, DesktopSection, L"ScreenSaveTimeOut", &timeout))
        ReadThemeUInt(theme_path, LegacySaverSection, L"ScreenSaveTimeOut", &timeout);
    if (!ReadThemeUInt(theme_path, DesktopSection, L"ScreenSaveActive", &active))
        ReadThemeUInt(theme_path, LegacySaverSection, L"ScreenSaveActive", &active);

    item->ScreensaverTimeout = timeout;
    item->ScreensaverActive = (item->Screensaver[0] != L'\0') && (active != 0);
}

/* Fill a theme item from a .theme file; the thumbnail is made by the caller */
static void
ReadThemeFile(const WCHAR *theme_path, const WCHAR *file_name, LTHEME_ITEM *item)
{
    WCHAR *dot;

    ZeroMemory(item, sizeof(*item));
    StringCchCopyW(item->ThemePath, ARRAYSIZE(item->ThemePath), theme_path);

    if (!ReadThemeString(theme_path, L"Theme", L"DisplayName", item->Name, ARRAYSIZE(item->Name)))
    {
        StringCchCopyW(item->Name, ARRAYSIZE(item->Name), file_name);
        dot = wcsrchr(item->Name, L'.');
        if (dot)
            *dot = L'\0';
    }

    ReadThemeWallpaper(theme_path, item);
    ReadThemeWindowColor(theme_path, item);
    ReadThemeString(theme_path, SoundsSection, L"SchemeName", item->SoundsScheme, ARRAYSIZE(item->SoundsScheme));
    ReadThemeScreensaver(theme_path, item);
}

/* Thumbnail **************************************************************/

/* Draw the image so that it covers the rectangle, cropping what overhangs */
static void
DrawImageCover(GpGraphics *graphics, GpImage *image, INT x, INT y, INT width, INT height)
{
    UINT image_width = 0, image_height = 0;
    INT src_x = 0, src_y = 0, src_w, src_h;

    if (GdipGetImageWidth(image, &image_width) != Ok || GdipGetImageHeight(image, &image_height) != Ok ||
        image_width == 0 || image_height == 0)
    {
        return;
    }

    src_w = (INT)image_width;
    src_h = (INT)image_height;
    if ((ULONGLONG)image_width * height > (ULONGLONG)image_height * width)
    {
        src_w = (INT)(((ULONGLONG)image_height * width) / height);
        src_x = ((INT)image_width - src_w) / 2;
    }
    else
    {
        src_h = (INT)(((ULONGLONG)image_width * height) / width);
        src_y = ((INT)image_height - src_h) / 2;
    }

    GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
    GdipDrawImageRectRectI(graphics, image, x, y, width, height, src_x, src_y, src_w, src_h,
                           UnitPixel, NULL, NULL, NULL);
}

/* Wallpaper preview (any format GDI+ decodes: BMP, JPEG, PNG, GIF, TIFF) + window color swatch */
HBITMAP WINAPI
LThemeCreateThumbnail(const WCHAR *wallpaper_path, COLORREF window_color)
{
    enum { THUMB_WIDTH = 200, THUMB_HEIGHT = 150, SWATCH_SIZE = 40, SWATCH_MARGIN = 8 };
    BITMAPINFO bmi;
    PVOID bits = NULL;
    HBITMAP hbm, old_bm;
    HDC hdc_mem;
    HBRUSH brush;
    RECT rc;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = THUMB_WIDTH;
    bmi.bmiHeader.biHeight = -THUMB_HEIGHT;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc_mem = CreateCompatibleDC(NULL);
    if (!hdc_mem)
        return NULL;

    hbm = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm)
    {
        DeleteDC(hdc_mem);
        return NULL;
    }
    old_bm = (HBITMAP)SelectObject(hdc_mem, hbm);

    /* Placeholder when there is no picture: a neutral desktop */
    SetRect(&rc, 0, 0, THUMB_WIDTH, THUMB_HEIGHT);
    brush = CreateSolidBrush(RGB(58, 110, 165));
    if (brush)
    {
        FillRect(hdc_mem, &rc, brush);
        DeleteObject(brush);
    }

    if (wallpaper_path && wallpaper_path[0])
    {
        struct GdiplusStartupInput input;
        ULONG_PTR token = 0;

        ZeroMemory(&input, sizeof(input));
        input.GdiplusVersion = 1;
        if (GdiplusStartup(&token, &input, NULL) == Ok)
        {
            GpImage *image = NULL;
            GpGraphics *graphics = NULL;

            if (GdipLoadImageFromFile(wallpaper_path, &image) == Ok &&
                GdipCreateFromHDC(hdc_mem, &graphics) == Ok)
            {
                DrawImageCover(graphics, image, 0, 0, THUMB_WIDTH, THUMB_HEIGHT);
            }
            if (graphics)
                GdipDeleteGraphics(graphics);
            if (image)
                GdipDisposeImage(image);
            GdiplusShutdown(token);
        }
    }

    /* Window color swatch, framed so it stays visible on any picture */
    SetRect(&rc, THUMB_WIDTH - SWATCH_SIZE - SWATCH_MARGIN, THUMB_HEIGHT - SWATCH_SIZE - SWATCH_MARGIN,
            THUMB_WIDTH - SWATCH_MARGIN, THUMB_HEIGHT - SWATCH_MARGIN);
    brush = CreateSolidBrush(RGB(255, 255, 255));
    if (brush)
    {
        FillRect(hdc_mem, &rc, brush);
        DeleteObject(brush);
    }
    InflateRect(&rc, -2, -2);
    brush = CreateSolidBrush(window_color);
    if (brush)
    {
        FillRect(hdc_mem, &rc, brush);
        DeleteObject(brush);
    }

    GdiFlush();
    SelectObject(hdc_mem, old_bm);
    DeleteDC(hdc_mem);
    return hbm;
}

/* Enumeration ************************************************************/

static BOOL
ExpandThemePath(const WCHAR *template_path, WCHAR *out_expanded, DWORD out_cch)
{
    DWORD expanded = ExpandEnvironmentStringsW(template_path, out_expanded, out_cch);
    return expanded > 0 && expanded <= out_cch && !wcschr(out_expanded, L'%');
}

static UINT
ScanThemesInDirectory(const WCHAR *dir_path, LTHEME_LIST *list)
{
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    WCHAR pattern[MAX_PATH];
    WCHAR theme_path[MAX_PATH];

    if (!dir_path || !dir_path[0])
        return list->count;

    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.theme", dir_path)))
        return list->count;

    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return list->count;

    do
    {
        LTHEME_ITEM *item;

        if (list->count >= list->capacity)
            break;
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (FAILED(StringCchPrintfW(theme_path, ARRAYSIZE(theme_path), L"%s\\%s", dir_path, find_data.cFileName)))
            continue;

        item = &list->items[list->count];
        ReadThemeFile(theme_path, find_data.cFileName, item);
        item->Thumbnail = LThemeCreateThumbnail(item->Wallpaper, item->WindowColor);
        list->count++;
    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
    return list->count;
}

HRESULT WINAPI
LThemeEnumerate(LTHEME_LIST *out)
{
    WCHAR system_themes_dir[MAX_PATH];
    WCHAR user_themes_dir[MAX_PATH];

    if (!out)
        return E_INVALIDARG;

    out->count = 0;
    out->capacity = 0;
    out->items = (LTHEME_ITEM *)calloc(LTHEME_MAX_ITEMS, sizeof(LTHEME_ITEM));
    if (!out->items)
        return E_OUTOFMEMORY;
    out->capacity = LTHEME_MAX_ITEMS;

    if (SUCCEEDED(LThemeGetUnsaved(&out->items[0])))
        out->count++;

    if (!ExpandThemePath(L"%SystemRoot%\\Resources\\Themes", system_themes_dir, ARRAYSIZE(system_themes_dir)))
        system_themes_dir[0] = L'\0';
    if (!ExpandThemePath(L"%APPDATA%\\Microsoft\\Windows\\Themes", user_themes_dir, ARRAYSIZE(user_themes_dir)))
        user_themes_dir[0] = L'\0';

    ScanThemesInDirectory(system_themes_dir, out);
    if (user_themes_dir[0] && lstrcmpiW(user_themes_dir, system_themes_dir) != 0)
        ScanThemesInDirectory(user_themes_dir, out);

    return S_OK;
}

void WINAPI
LThemeListFree(LTHEME_LIST *list)
{
    UINT i;

    if (!list || !list->items)
        return;

    for (i = 0; i < list->count; i++)
    {
        if (list->items[i].Thumbnail)
            DeleteObject(list->items[i].Thumbnail);
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Sound schemes **********************************************************/

/* The key name of the current scheme (HKCU\AppEvents\Schemes default value), e.g. ".Default" */
static void
GetCurrentSoundScheme(WCHAR *out_name, DWORD out_cch)
{
    DWORD size = out_cch * sizeof(WCHAR);
    DWORD type;
    HKEY key;

    out_name[0] = L'\0';
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SchemesKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)out_name, &size) != ERROR_SUCCESS ||
        type != REG_SZ)
    {
        out_name[0] = L'\0';
    }
    out_name[out_cch - 1] = L'\0';
    RegCloseKey(key);
}

/* A theme names a scheme by its key (".Default") or by its display name: find the key */
static BOOL
FindSoundSchemeKey(const WCHAR *scheme, WCHAR *out_key, DWORD out_cch)
{
    WCHAR name[128], display[256];
    DWORD index, name_cch, size, type;
    HKEY names, sub;
    BOOL found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Names", 0, KEY_READ, &names) != ERROR_SUCCESS)
        return FALSE;

    for (index = 0; !found; index++)
    {
        name_cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(names, index, name, &name_cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (!lstrcmpiW(name, scheme))
        {
            found = TRUE;
        }
        else if (RegOpenKeyExW(names, name, 0, KEY_QUERY_VALUE, &sub) == ERROR_SUCCESS)
        {
            size = sizeof(display) - sizeof(WCHAR);
            ZeroMemory(display, sizeof(display));
            if (RegQueryValueExW(sub, NULL, NULL, &type, (BYTE *)display, &size) == ERROR_SUCCESS &&
                type == REG_SZ && !lstrcmpiW(display, scheme))
            {
                found = TRUE;
            }
            RegCloseKey(sub);
        }

        if (found)
            StringCchCopyW(out_key, out_cch, name);
    }

    RegCloseKey(names);
    return found;
}

/* Make a scheme current, as the Sounds control panel does: record its name and copy each
 * event's sound of that scheme into the event's .Current value */
static BOOL
ApplySoundScheme(const WCHAR *scheme)
{
    WCHAR scheme_key[128], app[128], event[128], subkey[MAX_PATH], sound[MAX_PATH];
    DWORD app_index, event_index, cch, size, type;
    HKEY schemes, apps, app_key, event_key, source, current;

    if (!FindSoundSchemeKey(scheme, scheme_key, ARRAYSIZE(scheme_key)))
        return FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, SchemesKey, 0, KEY_SET_VALUE, &schemes) != ERROR_SUCCESS)
        return FALSE;
    RegSetValueExW(schemes, NULL, 0, REG_SZ, (const BYTE *)scheme_key,
                   (lstrlenW(scheme_key) + 1) * sizeof(WCHAR));
    RegCloseKey(schemes);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"AppEvents\\Schemes\\Apps", 0, KEY_READ, &apps) != ERROR_SUCCESS)
        return TRUE;

    for (app_index = 0; ; app_index++)
    {
        cch = ARRAYSIZE(app);
        if (RegEnumKeyExW(apps, app_index, app, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(apps, app, 0, KEY_READ, &app_key) != ERROR_SUCCESS)
            continue;

        for (event_index = 0; ; event_index++)
        {
            cch = ARRAYSIZE(event);
            if (RegEnumKeyExW(app_key, event_index, event, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExW(app_key, event, 0, KEY_READ, &event_key) != ERROR_SUCCESS)
                continue;

            /* An event the scheme does not name keeps no sound */
            sound[0] = L'\0';
            type = REG_SZ;
            if (RegOpenKeyExW(event_key, scheme_key, 0, KEY_QUERY_VALUE, &source) == ERROR_SUCCESS)
            {
                size = sizeof(sound) - sizeof(WCHAR);
                if (RegQueryValueExW(source, NULL, NULL, &type, (BYTE *)sound, &size) != ERROR_SUCCESS ||
                    (type != REG_SZ && type != REG_EXPAND_SZ))
                {
                    sound[0] = L'\0';
                    type = REG_SZ;
                }
                sound[ARRAYSIZE(sound) - 1] = L'\0';
                RegCloseKey(source);
            }

            if (SUCCEEDED(StringCchPrintfW(subkey, ARRAYSIZE(subkey), L"%s\\.Current", event)) &&
                RegCreateKeyExW(app_key, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &current, NULL) == ERROR_SUCCESS)
            {
                RegSetValueExW(current, NULL, 0, type, (const BYTE *)sound, (lstrlenW(sound) + 1) * sizeof(WCHAR));
                RegCloseKey(current);
            }
            RegCloseKey(event_key);
        }
        RegCloseKey(app_key);
    }

    RegCloseKey(apps);
    return TRUE;
}

/* Current settings and apply *********************************************/

HRESULT WINAPI
LThemeGetUnsaved(LTHEME_ITEM *out)
{
    WCHAR saver[MAX_PATH];
    UINT timeout = THEME_DEFAULT_TIMEOUT;
    BOOL active = FALSE;

    if (!out)
        return E_INVALIDARG;

    ZeroMemory(out, sizeof(*out));
    StringCchCopyW(out->Name, ARRAYSIZE(out->Name), UnsavedThemeName);

    /* No wallpaper is a valid current setting, not a failure */
    if (!LWallGetCurrent(out->Wallpaper, ARRAYSIZE(out->Wallpaper), &out->WallpaperStyle))
    {
        out->Wallpaper[0] = L'\0';
        out->WallpaperStyle = LWALL_CENTER;
    }

    out->WindowColor = GetSysColor(COLOR_ACTIVECAPTION);
    GetCurrentSoundScheme(out->SoundsScheme, ARRAYSIZE(out->SoundsScheme));

    if (LSaverGet(saver, ARRAYSIZE(saver), &timeout, &active))
    {
        StringCchCopyW(out->Screensaver, ARRAYSIZE(out->Screensaver), saver);
        out->ScreensaverTimeout = timeout;
        out->ScreensaverActive = active;
    }
    else
    {
        out->ScreensaverTimeout = THEME_DEFAULT_TIMEOUT;
    }

    out->Thumbnail = LThemeCreateThumbnail(out->Wallpaper, out->WindowColor);
    return S_OK;
}

HRESULT WINAPI
LThemeApply(const LTHEME_ITEM *theme)
{
    HRESULT hr = S_OK;
    WCHAR color_str[32];
    INT element = COLOR_ACTIVECAPTION;
    COLORREF color;
    HKEY key;

    if (!theme)
        return E_INVALIDARG;

    if (theme->Wallpaper[0] && !LWallSet(theme->Wallpaper, theme->WallpaperStyle))
        hr = HRESULT_FROM_WIN32(GetLastError());

    /* Window color: the live system color and the stored one */
    color = theme->WindowColor;
    if (SetSysColors(1, &element, &color))
    {
        StringCchPrintfW(color_str, ARRAYSIZE(color_str), L"%u %u %u",
                         GetRValue(color), GetGValue(color), GetBValue(color));
        if (RegCreateKeyExW(HKEY_CURRENT_USER, ColorsSection, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, L"ActiveTitle", 0, REG_SZ, (const BYTE *)color_str,
                           (lstrlenW(color_str) + 1) * sizeof(WCHAR));
            RegCloseKey(key);
        }
    }
    else if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }

    if (theme->SoundsScheme[0] && !ApplySoundScheme(theme->SoundsScheme) && SUCCEEDED(hr))
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    /* A theme without a screen saver turns it off, as on Windows */
    if (!LSaverSet(theme->Screensaver[0] ? theme->Screensaver : NULL,
                   theme->ScreensaverTimeout ? theme->ScreensaverTimeout : THEME_DEFAULT_TIMEOUT,
                   theme->Screensaver[0] ? theme->ScreensaverActive : FALSE) &&
        SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }

    return hr;
}

/* Saving *****************************************************************/

static BOOL
WriteThemeString(const WCHAR *theme_path, const WCHAR *section, const WCHAR *key, const WCHAR *value)
{
    return WritePrivateProfileStringW(section, key, value ? value : L"", theme_path);
}

static const WCHAR *
WallpaperStyleValue(LWALL_STYLE style)
{
    switch (style)
    {
        case LWALL_FILL:    return L"10";
        case LWALL_FIT:     return L"6";
        case LWALL_STRETCH: return L"2";
        default:            return L"0";
    }
}

/* Create a directory and the missing directories above it */
static BOOL
CreateDirectoryPath(const WCHAR *path)
{
    WCHAR partial[MAX_PATH];
    WCHAR *p;

    if (FAILED(StringCchCopyW(partial, ARRAYSIZE(partial), path)))
        return FALSE;

    /* Skip the drive, or the server and share of a UNC path */
    p = partial;
    if (p[0] == L'\\' && p[1] == L'\\')
    {
        p = wcschr(p + 2, L'\\');
        if (p)
            p = wcschr(p + 1, L'\\');
    }
    else
    {
        p = wcschr(p, L'\\');
    }

    while (p)
    {
        *p = L'\0';
        if (partial[0] && !CreateDirectoryW(partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
        *p = L'\\';
        p = wcschr(p + 1, L'\\');
    }

    return CreateDirectoryW(partial, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* A theme name becomes a file name: refuse what would leave the themes directory */
static BOOL
IsValidThemeName(const WCHAR *name)
{
    if (!name[0] || !lstrcmpW(name, L".") || !lstrcmpW(name, L".."))
        return FALSE;
    return wcspbrk(name, L"\\/:*?\"<>|") == NULL;
}

HRESULT WINAPI
LThemeSave(const WCHAR *theme_name, WCHAR out_path[MAX_PATH])
{
    WCHAR user_themes_dir[MAX_PATH];
    WCHAR theme_path[MAX_PATH];
    WCHAR value[32];
    LTHEME_ITEM current;
    HRESULT hr;

    if (!theme_name || !out_path || !IsValidThemeName(theme_name))
        return E_INVALIDARG;

    if (!ExpandThemePath(L"%APPDATA%\\Microsoft\\Windows\\Themes", user_themes_dir, ARRAYSIZE(user_themes_dir)))
        return E_FAIL;
    if (!CreateDirectoryPath(user_themes_dir))
        return HRESULT_FROM_WIN32(GetLastError());

    hr = StringCchPrintfW(theme_path, ARRAYSIZE(theme_path), L"%s\\%s.theme", user_themes_dir, theme_name);
    if (FAILED(hr))
        return hr;

    hr = LThemeGetUnsaved(&current);
    if (FAILED(hr))
        return hr;
    if (current.Thumbnail)
    {
        DeleteObject(current.Thumbnail);
        current.Thumbnail = NULL;
    }

    /* Start from an empty file so no stale key of an older theme survives */
    DeleteFileW(theme_path);

    if (!WriteThemeString(theme_path, L"Theme", L"DisplayName", theme_name))
        return HRESULT_FROM_WIN32(GetLastError());

    WriteThemeString(theme_path, DesktopSection, L"Wallpaper", current.Wallpaper);
    WriteThemeString(theme_path, DesktopSection, L"WallpaperStyle", WallpaperStyleValue(current.WallpaperStyle));
    WriteThemeString(theme_path, DesktopSection, L"TileWallpaper", current.WallpaperStyle == LWALL_TILE ? L"1" : L"0");

    StringCchPrintfW(value, ARRAYSIZE(value), L"%u %u %u",
                     GetRValue(current.WindowColor), GetGValue(current.WindowColor), GetBValue(current.WindowColor));
    WriteThemeString(theme_path, ColorsSection, L"ActiveTitle", value);

    WriteThemeString(theme_path, SoundsSection, L"SchemeName", current.SoundsScheme);

    /* Windows form of the screen saver */
    WriteThemeString(theme_path, BootSection, L"SCRNSAVE.EXE", current.Screensaver);
    StringCchPrintfW(value, ARRAYSIZE(value), L"%u", current.ScreensaverTimeout);
    WriteThemeString(theme_path, DesktopSection, L"ScreenSaveTimeOut", value);
    WriteThemeString(theme_path, DesktopSection, L"ScreenSaveActive", current.ScreensaverActive ? L"1" : L"0");

    WriteThemeString(theme_path, L"MasterThemeSelector", L"MTSM", L"DABJDKT");

    /* Flush the profile cache to disk */
    WritePrivateProfileStringW(NULL, NULL, NULL, theme_path);

    StringCchCopyW(out_path, MAX_PATH, theme_path);
    return S_OK;
}
