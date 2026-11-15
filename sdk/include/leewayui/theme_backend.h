/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Theme enumeration, thumbnail composite, and theme application
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The Control Panel item (dll/cpl/leewaydisp) uses this API to:
 * - Enumerate themes from %WINDIR%\Resources\Themes and user theme directory
 * - Create composite thumbnails (wallpaper preview + window color swatch)
 * - Apply a complete theme (wallpaper + window color + sounds + screensaver) in one call
 * - Get current settings as "Unsaved Theme"
 * - Save current settings as a new user theme
 *
 * Implementation: sdk/lib/leewaydisplay/theme_backend.c
 * .theme file format (Windows): INI with [Theme] DisplayName, [Control Panel\Desktop] Wallpaper/
 * WallpaperStyle/TileWallpaper/ScreenSaveTimeOut/ScreenSaveActive, [Control Panel\Colors] ActiveTitle,
 * [Sounds] SchemeName, [boot] SCRNSAVE.EXE. Paths may use environment variables (%SystemRoot%).
 * An older [Screen Saver] section is still read, never written.
 */

#pragma once

#include <windows.h>
#include <leewayui/display_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LTHEME_MAX_ITEMS 64

/* Theme descriptor: name, paths, current settings, and thumbnail */
typedef struct LTHEME_ITEM
{
    WCHAR Name[128];          /* Display name (e.g. "Windows 7", "Unsaved Theme") */
    WCHAR ThemePath[MAX_PATH]; /* Full path to .theme file, or empty if builtin/unsaved */
    HBITMAP Thumbnail;        /* HBITMAP 200x150, wallpaper preview + color swatch, or NULL */

    /* Current theme settings */
    WCHAR Wallpaper[MAX_PATH];
    LWALL_STYLE WallpaperStyle;
    COLORREF WindowColor;     /* Active Title Bar color (from Colors scheme) */
    WCHAR SoundsScheme[128];  /* Sounds scheme name, or empty if none */
    WCHAR Screensaver[MAX_PATH]; /* Screensaver .scr path, or empty if none */
    UINT ScreensaverTimeout;  /* Timeout in seconds, only valid if Screensaver is set */
    BOOL ScreensaverActive;   /* Is screensaver enabled */
} LTHEME_ITEM;

typedef struct LTHEME_LIST
{
    LTHEME_ITEM *items;
    UINT count;
    UINT capacity;
} LTHEME_LIST;

/*
 * Enumerate all available themes:
 * - Searches %WINDIR%\Resources\Themes\*.theme
 * - Searches %APPDATA%\Microsoft\Windows\Themes\*.theme
 * - First item is "Unsaved Theme" (current system settings)
 * - Returns a list with thumbnails already generated
 * Caller must free via LThemeListFree()
 */
HRESULT WINAPI LThemeEnumerate(LTHEME_LIST *out);

/*
 * Free the theme list and all associated resources (including HBITMAPs)
 */
void WINAPI LThemeListFree(LTHEME_LIST *list);

/*
 * Create a thumbnail HBITMAP for a theme:
 * - Loads the wallpaper with GDI+ (BMP, JPEG, PNG, GIF, TIFF) and covers the 200x150 canvas
 * - Overlays a framed window color swatch (40x40) in the bottom-right corner
 * - Returns HBITMAP 200x150 (caller must DeleteObject when done)
 * - If wallpaper cannot be loaded, returns a placeholder with color swatch only
 * Used internally by LThemeEnumerate, but also exposed for custom theme creation
 */
HBITMAP WINAPI LThemeCreateThumbnail(const WCHAR *wallpaper_path, COLORREF window_color);

/*
 * Apply a theme to the system:
 * - Sets wallpaper via LWallSet()
 * - Sets the window color (COLOR_ACTIVECAPTION, stored in HKCU\Control Panel\Colors ActiveTitle)
 * - Makes the sound scheme current as the Sounds control panel does (HKCU\AppEvents\Schemes and
 *   each event's .Current value); SoundsScheme is a scheme key (".Default") or display name
 * - Sets the screensaver via LSaverSet(); a theme without one turns the screen saver off
 * Returns S_OK on success, or HRESULT error (partial failure returns the first error)
 * All changes are immediate; use LDispApplyLayout/LDispCommitLayout for display changes
 */
HRESULT WINAPI LThemeApply(const LTHEME_ITEM *theme);

/*
 * Get the current system settings as an "Unsaved Theme":
 * - Reads current wallpaper via LWallGetCurrent()
 * - Reads current window color (COLOR_ACTIVECAPTION)
 * - Reads current sounds scheme key (HKCU\AppEvents\Schemes)
 * - Reads current screensaver via LSaverGet()
 * - Generates a thumbnail from current wallpaper + color
 * - Returns LTHEME_ITEM with Name="Unsaved Theme", ThemePath empty
 * Caller must DeleteObject() the thumbnail when done
 */
HRESULT WINAPI LThemeGetUnsaved(LTHEME_ITEM *out);

/*
 * Save the current system settings as a new user .theme file:
 * - Queries current wallpaper, window color, sounds, screensaver
 * - Writes %APPDATA%\Microsoft\Windows\Themes\<name>.theme in the Windows format above
 * - theme_name must be a valid file name (no path separators or reserved characters)
 * - Returns the full path to the saved theme in out_path
 * - On success, you can load this theme again via LThemeEnumerate()
 */
HRESULT WINAPI LThemeSave(const WCHAR *theme_name, WCHAR out_path[MAX_PATH]);

#ifdef __cplusplus
}
#endif
