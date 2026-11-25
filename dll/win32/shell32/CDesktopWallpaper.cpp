/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IDesktopWallpaper on the SystemParametersInfo and IActiveDesktop paths
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell_ad);

#define MAX_WALLPAPER_MONITORS 16

/* While disabled, the wallpaper Enable(TRUE) brings back; present only then. */
static const WCHAR szWallpapersKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";
static const WCHAR szDisabledValue[] = L"DisabledWallpaperPath";

typedef struct _WALLPAPER_MONITOR
{
    WCHAR szId[CCHDEVICENAME];  /* MONITORINFOEXW::szDevice */
    RECT rcMonitor;
} WALLPAPER_MONITOR;

typedef struct _WALLPAPER_MONITOR_LIST
{
    WALLPAPER_MONITOR *pList;
    UINT cMax;
    UINT cFound;
} WALLPAPER_MONITOR_LIST;

static HRESULT
HResultFromLastError(VOID)
{
    DWORD dwError = GetLastError();
    return dwError ? HRESULT_FROM_WIN32(dwError) : E_FAIL;
}

static BOOL CALLBACK
AddWallpaperMonitor(HMONITOR hMonitor, HDC hdc, LPRECT prc, LPARAM lParam)
{
    WALLPAPER_MONITOR_LIST *pList = reinterpret_cast<WALLPAPER_MONITOR_LIST *>(lParam);
    MONITORINFOEXW Info;

    Info.cbSize = sizeof(Info);
    if (GetMonitorInfoW(hMonitor, reinterpret_cast<LPMONITORINFO>(&Info)))
    {
        WALLPAPER_MONITOR *pEntry = &pList->pList[pList->cFound++];
        StringCchCopyW(pEntry->szId, ARRAYSIZE(pEntry->szId), Info.szDevice);
        pEntry->rcMonitor = Info.rcMonitor;
    }
    return pList->cFound < pList->cMax;
}

/* Monitors of the desktop, keyed by their display device name. Callers
 * treat the ID as opaque, so it need not be the device interface path. */
static UINT
EnumWallpaperMonitors(WALLPAPER_MONITOR *pList, UINT cMax)
{
    WALLPAPER_MONITOR_LIST List = { pList, cMax, 0 };

    if (cMax)
        EnumDisplayMonitors(NULL, NULL, AddWallpaperMonitor, reinterpret_cast<LPARAM>(&List));
    return List.cFound;
}

static INT
FindWallpaperMonitor(LPCWSTR pszId, const WALLPAPER_MONITOR *pList, UINT cList)
{
    for (UINT i = 0; i < cList; ++i)
    {
        if (!lstrcmpiW(pList[i].szId, pszId))
            return (INT)i;
    }
    return -1;
}

/* NULL addresses every monitor; anything else must name a current monitor. */
static HRESULT
CheckMonitorId(LPCWSTR pszId)
{
    WALLPAPER_MONITOR List[MAX_WALLPAPER_MONITORS];

    if (!pszId)
        return S_OK;
    if (FindWallpaperMonitor(pszId, List, EnumWallpaperMonitors(List, ARRAYSIZE(List))) < 0)
        return E_INVALIDARG;
    return S_OK;
}

static BOOL
GetDisabledWallpaper(LPWSTR pszPath, DWORD cchPath)
{
    WCHAR szLive[MAX_PATH];
    DWORD cb = cchPath * sizeof(WCHAR);
    HKEY hKey;

    if (RegGetValueW(HKEY_CURRENT_USER, szWallpapersKey, szDisabledValue,
                     RRF_RT_REG_SZ, NULL, pszPath, &cb) != ERROR_SUCCESS)
        return FALSE;

    /* Someone else (desk.cpl, a theme, any SPI caller) set a wallpaper since; drop the stale marker. */
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szLive), szLive, 0) && szLive[0] != UNICODE_NULL)
    {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, szWallpapersKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hKey, szDisabledValue);
            RegCloseKey(hKey);
        }
        return FALSE;
    }
    return TRUE;
}

static HRESULT
SetDisabledWallpaper(LPCWSTR pszPath)
{
    LSTATUS Status;
    HKEY hKey;

    Status = RegCreateKeyExW(HKEY_CURRENT_USER, szWallpapersKey, 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &hKey, NULL);
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);
    Status = RegSetValueExW(hKey, szDisabledValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(pszPath),
                            (lstrlenW(pszPath) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(Status);
}

static HRESULT
ApplyWallpaper(LPCWSTR pszPath)
{
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<LPWSTR>(pszPath),
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
    {
        HRESULT hr = HResultFromLastError();
        ERR("SPI_SETDESKWALLPAPER failed for '%ls': 0x%lx\n", pszPath, hr);
        return hr;
    }
    return S_OK;
}

static HRESULT
CreateActiveDesktop(IActiveDesktop **ppActiveDesktop)
{
    return ShellObjectCreator<CActiveDesktop>(IID_PPV_ARG(IActiveDesktop, ppActiveDesktop));
}

/***********************************************************************
*   IDesktopWallpaper implementation
*/

/* Win32k keeps a single wallpaper, so a monitor ID selects every monitor. */
HRESULT WINAPI CDesktopWallpaper::SetWallpaper(LPCWSTR monitorID, LPCWSTR wallpaper)
{
    WCHAR szDisabled[MAX_PATH];

    TRACE("(%ls, %ls)\n", monitorID, wallpaper);

    if (!wallpaper)
        return E_POINTER;
    if (lstrlenW(wallpaper) >= MAX_PATH)
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    /* SPI_SETDESKWALLPAPER succeeds for a file it cannot load (Win11 parity),
     * so a missing file is reported here instead of being committed. */
    if (wallpaper[0] != UNICODE_NULL)
    {
        DWORD dwAttributes = GetFileAttributesW(wallpaper);
        if (dwAttributes == INVALID_FILE_ATTRIBUTES)
        {
            /* Win11 answers ERROR_FILE_NOT_FOUND for a missing directory too. */
            if (GetLastError() == ERROR_PATH_NOT_FOUND)
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            return HResultFromLastError();
        }
        if (dwAttributes & FILE_ATTRIBUTE_DIRECTORY)
            return E_INVALIDARG;
    }

    /* Win11 checks the file first, then accepts an unknown monitor and changes nothing. */
    if (FAILED(CheckMonitorId(monitorID)))
        return S_OK;

    /* A disabled desktop stays blank; the new path waits for Enable(TRUE). */
    if (GetDisabledWallpaper(szDisabled, ARRAYSIZE(szDisabled)))
        return SetDisabledWallpaper(wallpaper);
    return ApplyWallpaper(wallpaper);
}

HRESULT WINAPI CDesktopWallpaper::GetWallpaper(LPCWSTR monitorID, LPWSTR *wallpaper)
{
    WCHAR szCurrent[MAX_PATH];
    HRESULT hr;

    TRACE("(%ls, %p)\n", monitorID, wallpaper);

    if (!wallpaper)
        return E_POINTER;
    *wallpaper = NULL;
    hr = CheckMonitorId(monitorID);
    if (FAILED(hr))
        return hr;

    /* Win11 keeps reporting the chosen wallpaper while the desktop is disabled. */
    if (GetDisabledWallpaper(szCurrent, ARRAYSIZE(szCurrent)))
        return SHStrDupW(szCurrent, wallpaper);
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szCurrent), szCurrent, 0))
        return HResultFromLastError();
    return SHStrDupW(szCurrent, wallpaper);
}

HRESULT WINAPI CDesktopWallpaper::GetMonitorDevicePathAt(UINT monitorIndex, LPWSTR *monitorID)
{
    WALLPAPER_MONITOR List[MAX_WALLPAPER_MONITORS];

    TRACE("(%u, %p)\n", monitorIndex, monitorID);

    if (!monitorID)
        return E_POINTER;
    *monitorID = NULL;
    if (monitorIndex >= EnumWallpaperMonitors(List, ARRAYSIZE(List)))
        return E_FAIL;  /* Win11 parity */
    return SHStrDupW(List[monitorIndex].szId, monitorID);
}

HRESULT WINAPI CDesktopWallpaper::GetMonitorDevicePathCount(UINT *count)
{
    WALLPAPER_MONITOR List[MAX_WALLPAPER_MONITORS];

    TRACE("(%p)\n", count);

    if (!count)
        return E_POINTER;
    *count = EnumWallpaperMonitors(List, ARRAYSIZE(List));
    return S_OK;
}

HRESULT WINAPI CDesktopWallpaper::GetMonitorRECT(LPCWSTR monitorID, RECT *displayRect)
{
    WALLPAPER_MONITOR List[MAX_WALLPAPER_MONITORS];
    INT iMonitor;

    TRACE("(%ls, %p)\n", monitorID, displayRect);

    if (!monitorID || !displayRect)
        return E_POINTER;
    SetRectEmpty(displayRect);

    iMonitor = FindWallpaperMonitor(monitorID, List, EnumWallpaperMonitors(List, ARRAYSIZE(List)));
    if (iMonitor < 0)
        return E_INVALIDARG;

    *displayRect = List[iMonitor].rcMonitor;
    return S_OK;
}

HRESULT WINAPI CDesktopWallpaper::SetBackgroundColor(COLORREF color)
{
    INT iElement = COLOR_DESKTOP;
    WCHAR szColor[16];
    LSTATUS Status;
    HKEY hKey;

    TRACE("(0x%08lx)\n", color);

    /* Win11 accepts and ignores the high byte. */
    color &= 0x00FFFFFF;
    if (!SetSysColors(1, &iElement, &color))
        return HResultFromLastError();

    /* SetSysColors only changes the session; persist it like the Display applet does. */
    StringCchPrintfW(szColor, ARRAYSIZE(szColor), L"%u %u %u",
                     GetRValue(color), GetGValue(color), GetBValue(color));
    Status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors", 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &hKey, NULL);
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);
    Status = RegSetValueExW(hKey, L"Background", 0, REG_SZ, reinterpret_cast<const BYTE *>(szColor),
                            (lstrlenW(szColor) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);
    return Status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(Status);
}

HRESULT WINAPI CDesktopWallpaper::GetBackgroundColor(COLORREF *color)
{
    TRACE("(%p)\n", color);

    if (!color)
        return E_POINTER;
    *color = GetSysColor(COLOR_DESKTOP);
    return S_OK;
}

HRESULT WINAPI CDesktopWallpaper::SetPosition(DESKTOP_WALLPAPER_POSITION position)
{
    CComPtr<IActiveDesktop> pActiveDesktop;
    WALLPAPEROPT Options = { sizeof(Options), WPSTYLE_CENTER };
    HRESULT hr;

    TRACE("(%d)\n", position);

    switch (position)
    {
        case DWPOS_CENTER:  Options.dwStyle = WPSTYLE_CENTER;     break;
        case DWPOS_TILE:    Options.dwStyle = WPSTYLE_TILE;       break;
        case DWPOS_STRETCH: Options.dwStyle = WPSTYLE_STRETCH;    break;
        case DWPOS_FIT:     Options.dwStyle = WPSTYLE_KEEPASPECT; break;
        case DWPOS_FILL:    Options.dwStyle = WPSTYLE_CROPTOFIT;  break;
        case DWPOS_SPAN:
            /* Win32k has no span mode yet (see SpiLoadWallpaperMode). */
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        default:
            return E_INVALIDARG;
    }

    hr = CreateActiveDesktop(&pActiveDesktop);
    if (FAILED(hr))
        return hr;
    hr = pActiveDesktop->SetWallpaperOptions(&Options, 0);
    if (FAILED(hr))
        return hr;

    /* Win32k reads the mode when it loads the bitmap, so reapply the wallpaper. */
    hr = pActiveDesktop->ApplyChanges(AD_APPLY_SAVE | AD_APPLY_REFRESH);
    return FAILED(hr) ? hr : S_OK;
}

HRESULT WINAPI CDesktopWallpaper::GetPosition(DESKTOP_WALLPAPER_POSITION *position)
{
    CComPtr<IActiveDesktop> pActiveDesktop;
    WALLPAPEROPT Options = { sizeof(Options), WPSTYLE_CENTER };
    HRESULT hr;

    TRACE("(%p)\n", position);

    if (!position)
        return E_POINTER;

    hr = CreateActiveDesktop(&pActiveDesktop);
    if (SUCCEEDED(hr))
        hr = pActiveDesktop->GetWallpaperOptions(&Options, 0);
    if (FAILED(hr))
        return hr;

    switch (Options.dwStyle)
    {
        case WPSTYLE_TILE:       *position = DWPOS_TILE;    break;
        case WPSTYLE_STRETCH:    *position = DWPOS_STRETCH; break;
        case WPSTYLE_KEEPASPECT: *position = DWPOS_FIT;     break;
        case WPSTYLE_CROPTOFIT:  *position = DWPOS_FILL;    break;
        default:                 *position = DWPOS_CENTER;  break;
    }
    return S_OK;
}

/* Slideshow support is step B. */
HRESULT WINAPI CDesktopWallpaper::SetSlideshow(IShellItemArray *items)
{
    FIXME("(%p): slideshow not implemented\n", items);
    return E_NOTIMPL;
}

/* With no slideshow configured, the getters answer like Win11. */
HRESULT WINAPI CDesktopWallpaper::GetSlideshow(IShellItemArray **items)
{
    TRACE("(%p)\n", items);

    if (!items)
        return E_POINTER;
    *items = NULL;
    return S_FALSE;
}

HRESULT WINAPI CDesktopWallpaper::SetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS options, UINT slideshowTick)
{
    FIXME("(0x%x, %u): slideshow not implemented\n", options, slideshowTick);
    return E_NOTIMPL;
}

HRESULT WINAPI CDesktopWallpaper::GetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS *options, UINT *slideshowTick)
{
    TRACE("(%p, %p)\n", options, slideshowTick);

    if (!options || !slideshowTick)
        return E_POINTER;
    /* Win11 reports its 30-minute default while no slideshow is configured. */
    *options = (DESKTOP_SLIDESHOW_OPTIONS)0;
    *slideshowTick = 30 * 60 * 1000;
    return S_OK;
}

HRESULT WINAPI CDesktopWallpaper::AdvanceSlideshow(LPCWSTR monitorID, DESKTOP_SLIDESHOW_DIRECTION direction)
{
    TRACE("(%ls, %d)\n", monitorID, direction);
    return E_UNEXPECTED;
}

HRESULT WINAPI CDesktopWallpaper::GetStatus(DESKTOP_SLIDESHOW_STATE *state)
{
    WCHAR szDisabled[MAX_PATH];

    TRACE("(%p)\n", state);

    if (!state)
        return E_POINTER;
    /* No slideshow can run yet, so only DSS_ENABLED applies. */
    if (GetDisabledWallpaper(szDisabled, ARRAYSIZE(szDisabled)))
        *state = (DESKTOP_SLIDESHOW_STATE)0;
    else
        *state = DSS_ENABLED;
    return S_OK;
}

/* Like Win11, disabling empties both the live and the saved wallpaper; the
 * chosen path is kept aside so GetWallpaper and Enable(TRUE) still see it. */
HRESULT WINAPI CDesktopWallpaper::Enable(BOOL enable)
{
    WCHAR szWallpaper[MAX_PATH];
    HKEY hKey;
    HRESULT hr;

    TRACE("(%d)\n", enable);

    if (!GetDisabledWallpaper(szWallpaper, ARRAYSIZE(szWallpaper)))
    {
        if (enable)
            return S_OK;
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szWallpaper), szWallpaper, 0))
            return HResultFromLastError();
        hr = SetDisabledWallpaper(szWallpaper);
        if (FAILED(hr))
            return hr;
        return ApplyWallpaper(L"");
    }
    if (!enable)
        return S_OK;

    hr = ApplyWallpaper(szWallpaper);
    if (FAILED(hr))
        return hr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, szWallpapersKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hKey, szDisabledValue);
        RegCloseKey(hKey);
    }
    return S_OK;
}
