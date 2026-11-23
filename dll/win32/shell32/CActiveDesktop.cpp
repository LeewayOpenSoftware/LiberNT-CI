/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IActiveDesktop wallpaper contracts on the user32 SPI path
 * COPYRIGHT:   Copyright 2018 Mark Jansen (mark.jansen@reactos.org)
 */

#include "precomp.h"

#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell_ad);

/* Public, documented IActiveDesktop constants (the tree's shlobj.h declares the
 * interface without its flag/style values). */
#define AD_APPLY_SAVE   0x00000001
#define AD_APPLY_ALL    0x00000002
#define AD_APPLY_FORCE  0x00000004

#define WPSTYLE_TILE    0
#define WPSTYLE_CENTER  1
#define WPSTYLE_STRETCH 2
#define WPSTYLE_FIT     3
#define WPSTYLE_FILL    4

static const WCHAR szDesktopKey[] = L"Control Panel\\Desktop";
static const WCHAR szWallpaperValue[] = L"Wallpaper";
static const WCHAR szWallpaperStyleValue[] = L"WallpaperStyle";
static const WCHAR szTileWallpaperValue[] = L"TileWallpaper";

/* Map a WPSTYLE value onto the registry pair win32k reads back through
 * SpiLoadWallpaperMode (win32ss/user/ntuser/sysparams.c). */
static BOOL
StyleToRegistry(DWORD dwStyle, LPCWSTR *ppszWallpaperStyle, LPCWSTR *ppszTileWallpaper)
{
    switch (dwStyle)
    {
        case WPSTYLE_TILE:    *ppszWallpaperStyle = L"0";  *ppszTileWallpaper = L"1"; return TRUE;
        case WPSTYLE_CENTER:  *ppszWallpaperStyle = L"0";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_STRETCH: *ppszWallpaperStyle = L"2";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_FIT:     *ppszWallpaperStyle = L"6";  *ppszTileWallpaper = L"0"; return TRUE;
        case WPSTYLE_FILL:    *ppszWallpaperStyle = L"10"; *ppszTileWallpaper = L"0"; return TRUE;
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

    if (!lstrcmpW(pszWallpaperStyle, L"2"))  { *pdwStyle = WPSTYLE_STRETCH; return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"6"))  { *pdwStyle = WPSTYLE_FIT;     return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"10")) { *pdwStyle = WPSTYLE_FILL;    return TRUE; }
    if (!lstrcmpW(pszWallpaperStyle, L"0"))  { *pdwStyle = WPSTYLE_CENTER;  return TRUE; }
    return FALSE;
}

static BOOL
IsValidStyle(DWORD dwStyle)
{
    return dwStyle <= WPSTYLE_FILL;
}

static HRESULT
ReadRegistryString(LPCWSTR pszValue, LPWSTR pszBuffer, DWORD cchBuffer)
{
    DWORD cb = cchBuffer * sizeof(WCHAR);

    pszBuffer[0] = UNICODE_NULL;
    if (RegGetValueW(HKEY_CURRENT_USER, szDesktopKey, pszValue,
                     RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, pszBuffer, &cb) != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

static HRESULT
WriteRegistryString(HKEY hKey, LPCWSTR pszValue, LPCWSTR pszData)
{
    if (RegSetValueExW(hKey, pszValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(pszData),
                       (lstrlenW(pszData) + 1) * sizeof(WCHAR)) != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
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
    HRESULT hr;

    TRACE("(%lx)\n", dwFlags);

    if (m_bOptionsPending)
    {
        LPCWSTR pszWallpaperStyle, pszTileWallpaper;

        if (!StyleToRegistry(m_dwPendingStyle, &pszWallpaperStyle, &pszTileWallpaper))
            return E_INVALIDARG;

        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, szDesktopKey, 0, NULL,
                            0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(GetLastError());
        hr = WriteRegistryString(hKey, szWallpaperStyleValue, pszWallpaperStyle);
        if (SUCCEEDED(hr))
            hr = WriteRegistryString(hKey, szTileWallpaperValue, pszTileWallpaper);
        RegCloseKey(hKey);
        if (FAILED(hr))
            return hr;
        m_bOptionsPending = FALSE;
    }

    if (dwFlags & (AD_APPLY_SAVE | AD_APPLY_ALL | AD_APPLY_FORCE))
    {
        WCHAR szCurrent[MAX_PATH];

        if (m_bWallpaperPending)
        {
            StringCchCopyW(szCurrent, ARRAYSIZE(szCurrent), m_szPendingWallpaper);
            m_bWallpaperPending = FALSE;
        }
        else
        {
            ReadRegistryString(szWallpaperValue, szCurrent, ARRAYSIZE(szCurrent));
        }

        /* Delegate decode/transcode/paint to the user32/win32k pipeline. */
        if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, szCurrent,
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    /* Read back the effective style so m_dwPendingStyle tracks reality. */
    if (SUCCEEDED(ReadRegistryString(szWallpaperStyleValue, szStyle, ARRAYSIZE(szStyle))) &&
        SUCCEEDED(ReadRegistryString(szTileWallpaperValue, szTile, ARRAYSIZE(szTile))))
    {
        RegistryToStyle(szStyle, szTile, &m_dwPendingStyle);
    }

    return S_OK;
}

HRESULT WINAPI CActiveDesktop::GetWallpaper(PWSTR pwszWallpaper, UINT cchWallpaper, DWORD dwFlags)
{
    WCHAR szCurrent[MAX_PATH];

    TRACE("(%p, %u, %lx)\n", pwszWallpaper, cchWallpaper, dwFlags);

    if (!pwszWallpaper || !cchWallpaper)
        return E_INVALIDARG;

    if ((dwFlags & 0x3) == 0x2 /* AD_GETWP_PENDING */ && m_bWallpaperPending)
    {
        StringCchCopyW(pwszWallpaper, cchWallpaper, m_szPendingWallpaper);
        return S_OK;
    }

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

HRESULT WINAPI CActiveDesktop::AddDesktopItemWithUI(HWND hwnd, LPCCOMPONENT pcomp, DWORD dwReserved)
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

HRESULT WINAPI CActiveDesktop::GetDesktopItem(int nComponent, LPCCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemByID(ULONG_PTR dwID, LPCCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GenerateDesktopItemHtml(PCWSTR pwszFileName, LPCCOMPONENT pcomp, DWORD dwReserved)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::AddUrl(HWND hwnd, PCWSTR pszSource, LPCCOMPONENT pcomp, DWORD dwFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI CActiveDesktop::GetDesktopItemBySource(PCWSTR pwszSource, LPCCOMPONENT pcomp, DWORD dwReserved)
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
