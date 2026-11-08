/*
 * LeewayUI Theme — light/dark/high-contrast support
 * LGPL-2.1+
 */

#pragma once

#include <windows.h>

struct ILeewayTheme;
typedef struct ILeewayTheme ILeewayTheme;

/* Theme creation */
HRESULT LeewayThemeCreate(ILeewayTheme **out_theme);
void LeewayThemeDestroy(ILeewayTheme *theme);

/* Theme state queries */
typedef enum {
    LEEWAY_THEME_MODE_LIGHT,
    LEEWAY_THEME_MODE_DARK,
    LEEWAY_THEME_MODE_HIGH_CONTRAST,
    LEEWAY_THEME_MODE_CLASSIC
} LeewayThemeMode;

LeewayThemeMode LeewayThemeGetMode(ILeewayTheme *theme);

/* Named color brushes (on-demand, cached) */
HBRUSH LeewayThemeGetBrush(ILeewayTheme *theme, const WCHAR *name);

/* Notify theme of system changes */
void LeewayThemeOnSettingChange(ILeewayTheme *theme);
void LeewayThemeOnThemeChange(ILeewayTheme *theme);
