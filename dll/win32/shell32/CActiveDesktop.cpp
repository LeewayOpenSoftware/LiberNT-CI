/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IActiveDesktop wallpaper contracts on the user32 SPI path
 * COPYRIGHT:   Copyright 2018 Mark Jansen (mark.jansen@reactos.org)
 */

#include "precomp.h"

#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell_ad);

static const WCHAR szDesktopKey[] = L"Control Panel\\Desktop";
static const WCHAR szWallpaperValue[] = L"Wallpaper";
static const WCHAR szWallpaperStyleValue[] = L"WallpaperStyle";
static const WCHAR szTileWallpaperValue[] = L"TileWallpaper";

/* Map a WPSTYLE value onto the registry pair win32k reads back through
 * SpiLoadWallpaperMode (win32ss/user/ntuser/sysparams.c). WPSTYLE_SPAN has
 * no Orosi equivalent yet and is rejected. */
static BOOL
StyleToRegistry(DWORD dwStyle, LPCWSTR *ppszWallpaperStyle, LPCWSTR *ppszTileWallpaper)
{
    switch (dwStyle)
    {
        case WPSTYLE_CENTER:     *ppszWallpaperStyle = L"0";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_TILE:       *ppszWallpaperStyle = L"0";  *ppszTileWallpaper = L"1"; return TRUE;
        case WPSTYLE_STRETCH:    *ppszWallpaperStyle = L"2";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_KEEPASPECT: *ppszWallpaperStyle = L"6";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_CROPTOFIT:  *ppszWallpaperStyle = L"10"; *ppszTileWallpaper = L"0"; return TRUE;
        default: return FALSE;
    }
}

static BOOL
RegistryToStyle(LPCWSTR pszWallpaperStyle, LPCWSTR pszTileWallpaper, DWORD *pdwStyle)
{
    if (pszTileWallpaper[0] == L'1' && pszWallpaperStyle[0] == L'0')
    {
        *pdwStyle = WPSTYLE_TILE;
        return TRUE;
    }
    if (pszTileWallpaper[0] != L'0')
        return FALSE;

    if (!lstrcmpW(pszWallpaperStyle, L"2"))  { *pdwStyle = WPSTYLE_STRETCH;    return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"6"))  { *pdwStyle = WPSTYLE_KEEPASPECT; return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"10")) { *pdwStyle = WPSTYLE_CROPTOFIT;  return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"0"))  { *pdwStyle = WPSTYLE_CENTER;     return TRUE; }
    return FALSE;
}

static BOOL
IsValidStyle(DWORD dwStyle)
{
    return dwStyle <= WPSTYLE_CROPTOFIT;
}

static HRESULT
ReadRegistryString(LPCWSTR pszValue, LPWSTR pszBuffer, DWORD cchBuffer)
{
    LSTATUS Status;
    DWORD cb = cchBuffer * sizeof(WCHAR);

    pszBuffer[0] = UNICODE_NULL;
    Status = RegGetValueW(HKEY_CURRENT_USER, szDesktopKey, pszValue,
                          RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, pszBuffer, &cb);
    if (Status != ERROR_SUCCESS)
    {
        pszBuffer[0] = UNICODE_NULL;
        return HRESULT_FROM_WIN32(Status);
    }
    return S_OK;
}

static HRESULT
WriteRegistryString(HKEY hKey, LPCWSTR pszValue, LPCWSTR pszData)
{
    LSTATUS Status = RegSetValueExW(hKey, pszValue, 0, REG_SZ,
                                    reinterpret_cast<const BYTE *>(pszData),
                                    (lstrlenW(pszData) + 1) * sizeof(WCHAR));
    return Status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(Status);
}

/***********************************************************************
*   IActiveDesktop implementation
*/

CActiveDesktop::CActiveDesktop() :
    m_dwPendingStyle(WPSTYLE_CENTER),
    m_bWallpaperPending(FALSE),
    m_bOptionsPending(FALSE)
{
    m_szPendingWallpaper[0] = UNICODE_NULL;
}

CActiveDesktop::~CActiveDesktop()
{
}

HRESULT WINAPI CActiveDesktop::ApplyChanges(DWORD dwFlags)
{
    WCHAR szStyle[16], szTile[16];
    HRESULT hr = S_OK;

    TRACE("(%lx)\n", dwFlags);

    /* AD_APPLY_SAVE persists the pending options (and a pending wallpaper
     * path, so SAVE alone does not silently drop it). */
    if (dwFlags & AD_APPLY_SAVE)
    {
        LPCWSTR pszWallpaperStyle, pszTileWallpaper;
        HKEY hKey;
        LSTATUS Status;

        if (m_bOptionsPending && !StyleToRegistry(m_dwPendingStyle, &pszWallpaperStyle, &pszTileWallpaper))
            return E_INVALIDARG;

        Status = RegCreateKeyExW(HKEY_CURRENT_USER, szDesktopKey, 0, NULL,
                                 0, KEY_WRITE, NULL, &hKey, NULL);
        if (Status != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(Status);

        hr = S_OK;
        if (m_bOptionsPending)
        {
            hr = WriteRegistryString(hKey, szWallpaperStyleValue, pszWallpaperStyle);
            if (SUCCEEDED(hr))
                hr = WriteRegistryString(hKey, szTileWallpaperValue, pszTileWallpaper);
            if (SUCCEEDED(hr))
                m_bOptionsPending = FALSE;
        }
        if (SUCCEEDED(hr) && m_bWallpaperPending)
        {
            hr = WriteRegistryString(hKey, szWallpaperValue, m_szPendingWallpaper);
            /* When REFRESH/FORCE follows, the apply branch below must still
             * see the pending value (e.g. SetWallpaper(L"") + AD_APPLY_ALL);
             * only consume it here when the call stops at persisting. */
            if (SUCCEEDED(hr) && !(dwFlags & (AD_APPLY_REFRESH | AD_APPLY_FORCE)))
                m_bWallpaperPending = FALSE;
        }
        RegCloseKey(hKey);
        if (FAILED(hr))
            return hr;
    }

    /* AD_APPLY_REFRESH / AD_APPLY_FORCE apply the wallpaper through SPI. */
    if (dwFlags & (AD_APPLY_REFRESH | AD_APPLY_FORCE))
    {
        WCHAR szCurrent[MAX_PATH];

        if (m_bWallpaperPending)
        {
            /* An explicit SetWallpaper() (including an empty string, which
             * clears the wallpaper) is committed as requested. */
            StringCchCopyW(szCurrent, ARRAYSIZE(szCurrent), m_szPendingWallpaper);
            m_bWallpaperPending = FALSE;

            if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, szCurrent,
                                       SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
                return HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            HRESULT hrRead = ReadRegistryString(szWallpaperValue, szCurrent, ARRAYSIZE(szCurrent));

            if (FAILED(hrRead))
            {
                if (hrRead == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                {
                    TRACE("No wallpaper configured; nothing to apply\n");
                    hr = S_FALSE;
                }
                else
                {
                    return hrRead;
                }
            }
            else if (szCurrent[0] != UNICODE_NULL)
            {
                if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, szCurrent,
                                           SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
                    return HRESULT_FROM_WIN32(GetLastError());
            }
            else
            {
                TRACE("No wallpaper configured; nothing to apply\n");
                hr = S_FALSE;
            }
        }
    }

    /* Read back the effective style so m_dwPendingStyle tracks reality, but
     * never clobber a pending style that has not been saved yet. */
    if (!m_bOptionsPending &&
        SUCCEEDED(ReadRegistryString(szWallpaperStyleValue, szStyle, ARRAYSIZE(szStyle))) &&
        SUCCEEDED(ReadRegistryString(szTileWallpaperValue, szTile, ARRAYSIZE(szTile))))
    {
        RegistryToStyle(szStyle, szTile, &m_dwPendingStyle);
    }

    return hr;
}

HRESULT WINAPI CActiveDesktop::GetWallpaper(PWSTR pwszWallpaper, UINT cchWallpaper, DWORD dwFlags)
{
    WCHAR szCurrent[MAX_PATH];

    TRACE("(%p, %u, %lx)\n", pwszWallpaper, cchWallpaper, dwFlags);

    if (!pwszWallpaper || !cchWallpaper)
        return E_INVALIDARG;
    if (dwFlags & ~(AD_GETWP_BMP | AD_GETWP_IMAGE | AD_GETWP_LAST_APPLIED))
        return E_INVALIDARG;

    /* BMP, IMAGE and LAST_APPLIED all resolve to the applied image path here
     * (there is no separate HTML/DS image to report). */
    ReadRegistryString(szWallpaperValue, szCurrent, ARRAYSIZE(szCurrent));
    StringCchCopyW(pwszWallpaper, cchWallpaper, szCurrent);
    return S_OK;
}

HRESULT WINAPI CActiveDesktop::SetWallpaper(PCWSTR pwszWallpaper, DWORD dwReserved)
{
    TRACE("(%ls, %lx)\n", pwszWallpaper, dwReserved);

    if (!pwszWallpaper)
        return E_INVALIDARG;

    /* Empty string clears the wallpaper; commit happens on ApplyChanges. */
    StringCchCopyW(m_szPendingWallpaper, ARRAYSIZE(m_szPendingWallpaper), pwszWallpaper);
    m_bWallpaperPending = TRUE;
    return S_OK;
}

HRESULT WINAPI CActiveDesktop::GetWallpaperOptions(LPWALLPAPEROPT pwpo, DWORD dwReserved)
{
    WCHAR szStyle[16], szTile[16];

    TRACE("(%p, %lx)\n", pwpo, dwReserved);

    if (!pwpo || pwpo->dwSize != sizeof(WALLPAPEROPT))
        return E_INVALIDARG;

    if (SUCCEEDED(ReadRegistryString(szWallpaperStyleValue, szStyle, ARRAYSIZE(szStyle))) &&
        SUCCEEDED(ReadRegistryString(szTileWallpaperValue, szTile, ARRAYSIZE(szTile))) &&
        RegistryToStyle(szStyle, szTile, &pwpo->dwStyle))
        return S_OK;

    pwpo->dwStyle = WPSTYLE_CENTER;
    return S_OK;
}

HRESULT WINAPI CActiveDesktop::SetWallpaperOptions(LPCWALLPAPEROPT pwpo, DWORD dwReserved)
{
    TRACE("(%p, %lx)\n", pwpo, dwReserved);

    if (!pwpo || pwpo->dwSize != sizeof(WALLPAPEROPT))
        return E_INVALIDARG;
    if (!IsValidStyle(pwpo->dwStyle))
        return E_INVALIDARG;

    m_dwPendingStyle = pwpo->dwStyle;
    m_bOptionsPending = TRUE;
    return S_OK;
}

HRESULT WINAPI CActiveDesktop::GetPattern(PWSTR pwszPattern, UINT cchPattern, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::SetPattern(PCWSTR pwszPattern, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemOptions(LPCOMPONENTSOPT pco, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::SetDesktopItemOptions(LPCCOMPONENTSOPT pco, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::AddDesktopItem(LPCCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::AddDesktopItemWithUI(HWND hwnd, LPCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::ModifyDesktopItem(LPCCOMPONENT pcomp, DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::RemoveDesktopItem(LPCCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemCount(int *pcItems, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItem(int nComponent, LPCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemByID(ULONG_PTR dwID, LPCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GenerateDesktopItemHtml(PCWSTR pwszFileName, LPCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::AddUrl(HWND hwnd, PCWSTR pszSource, LPCOMPONENT pcomp, DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemBySource(PCWSTR pwszSource, LPCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::SetSafeMode(DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::EnsureUpdateHTML()
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::SetScheme(PCWSTR pwszSchemeName, DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetScheme(PWSTR pwszSchemeName, DWORD *pdwcchBuffer, DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::ReReadWallpaper()
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetADObjectFlags(DWORD *pdwFlags, DWORD dwMask)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::UpdateAllDesktopSubscriptions()
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::MakeDynamicChanges(IOleObject *pOleObj)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}


/***********************************************************************
*   IPropertyBag implementation
*/

HRESULT WINAPI CActiveDesktop::Read(LPCOLESTR pszPropName, VARIANT *pVar, IErrorLog *pErrorLog)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::Write(LPCOLESTR pszPropName, VARIANT *pVar)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}
