/*
 * LeewayUI Theme — minimal skeleton implementation
 * LGPL-2.1+
 */

#include <windows.h>
#include <leewayui/theme.h>

struct ILeewayTheme {
    LeewayThemeMode current_mode;
    HBRUSH background_brush;
};

HRESULT LeewayThemeCreate(ILeewayTheme **out_theme)
{
    if (!out_theme) {
        return E_INVALIDARG;
    }

    ILeewayTheme *theme = (ILeewayTheme *)HeapAlloc(GetProcessHeap(), 0, sizeof(ILeewayTheme));
    if (!theme) {
        return E_OUTOFMEMORY;
    }

    /* Default to light theme */
    theme->current_mode = LEEWAY_THEME_MODE_LIGHT;
    theme->background_brush = CreateSolidBrush(RGB(255, 255, 255));

    *out_theme = theme;
    return S_OK;
}

void LeewayThemeDestroy(ILeewayTheme *theme)
{
    if (!theme) {
        return;
    }

    if (theme->background_brush) {
        DeleteObject(theme->background_brush);
    }

    HeapFree(GetProcessHeap(), 0, theme);
}

LeewayThemeMode LeewayThemeGetMode(ILeewayTheme *theme)
{
    return theme ? theme->current_mode : LEEWAY_THEME_MODE_LIGHT;
}

HBRUSH LeewayThemeGetBrush(ILeewayTheme *theme, const WCHAR *name)
{
    if (!theme || !name) {
        return NULL;
    }

    /* Skeleton: just return the background brush for any name */
    return theme->background_brush;
}

void LeewayThemeOnSettingChange(ILeewayTheme *theme)
{
    /* Skeleton: no-op; in real implementation would re-query system theme */
}

void LeewayThemeOnThemeChange(ILeewayTheme *theme)
{
    /* Skeleton: no-op; in real implementation would refresh theme state */
}
