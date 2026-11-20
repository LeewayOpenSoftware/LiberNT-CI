/*
 * PROJECT:     LiberNT Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Thumbnail provider for font files
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include "undocgdi.h" // for GetFontResourceInfoW

WINE_DEFAULT_DEBUG_CHANNEL(fontext);

const GUID CLSID_CFontThumbnail = { 0xe2537d60, 0x39c7, 0x4e40, { 0x95, 0x60, 0xb3, 0x38, 0x6d, 0x4b, 0xb2, 0x07 } };

STDMETHODIMP CFontThumbnail::Initialize(LPCWSTR pszFilePath, DWORD grfMode)
{
    if (!pszFilePath || !*pszFilePath)
        return E_INVALIDARG;
    m_Path = pszFilePath;
    return S_OK;
}

/*
 * A thumbnail parses the font in the kernel font engine without any click, so
 * only files from trusted locations get one: the system directory (the Fonts
 * folder lives there) and local fixed drives, and never a file that carries a
 * Zone.Identifier stream (downloaded). Removable and network files keep their
 * icon.
 */
static BOOL IsTrustedFontLocation(LPCWSTR pszPath)
{
    WCHAR szRoot[MAX_PATH];
    WCHAR szDrive[4];
    UINT cch;
    HANDLE hStream;
    CStringW strStream;

    if (!pszPath || !*pszPath || PathIsUNCW(pszPath))
        return FALSE;

    cch = GetWindowsDirectoryW(szRoot, _countof(szRoot));
    if (cch && cch < _countof(szRoot) && StrCmpNIW(pszPath, szRoot, cch) == 0 &&
        (pszPath[cch] == L'\\' || pszPath[cch] == UNICODE_NULL))
        return TRUE;

    if (!pszPath[0] || pszPath[1] != L':')
        return FALSE;
    szDrive[0] = pszPath[0];
    szDrive[1] = L':';
    szDrive[2] = L'\\';
    szDrive[3] = UNICODE_NULL;
    if (GetDriveTypeW(szDrive) != DRIVE_FIXED)
        return FALSE;

    strStream = pszPath;
    strStream += L":Zone.Identifier";
    hStream = CreateFileW(strStream, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hStream != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStream);
        TRACE("'%S' carries a Zone.Identifier stream: no thumbnail\n", pszPath);
        return FALSE;
    }
    return TRUE;
}

/*
 * The sample text in the face itself on a white tile, as the Fonts folder
 * shows it. The file is loaded as a private, non-enumerable resource for the
 * duration of the call, so it never shows up as an installed font; the font
 * mapper searches the process-private list first, so the file's own face is
 * the one drawn even when an installed face has the same name.
 */
STDMETHODIMP CFontThumbnail::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)
{
    LOGFONTW Logfonts[16];
    DWORD Size = sizeof(Logfonts);
    BITMAPINFO bmi;
    RGBQUAD *pBits = NULL;
    HDC hdc;
    HBITMAP hbm;
    HGDIOBJ hbmOld, hFontOld;
    HFONT hFont;
    LOGFONTW lf;
    RECT rc;
    BOOL bPrivate = FALSE;
    HRESULT hr = E_FAIL;

    if (pdwAlpha)
        *pdwAlpha = WTSAT_UNKNOWN;
    if (!phbmp)
        return E_POINTER;
    *phbmp = NULL;
    if (m_Path.IsEmpty() || !cx || cx > 4096)
        return E_FAIL;
    if (!IsTrustedFontLocation(m_Path))
        return E_FAIL;

    bPrivate = AddFontResourceExW(m_Path, FR_PRIVATE | FR_NOT_ENUM, NULL) != 0;
    if (!bPrivate || !GetFontResourceInfoW(m_Path, &Size, Logfonts, 2) || Size < sizeof(LOGFONTW))
    {
        ERR("Cannot load '%S' as a private font resource (%lu)\n", (LPCWSTR)m_Path, GetLastError());
        goto done;
    }

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -(LONG)cx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
        goto done;
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
    if (!hbm || !pBits)
    {
        if (hbm)
            DeleteObject(hbm);
        DeleteDC(hdc);
        hr = E_OUTOFMEMORY;
        goto done;
    }
    FillMemory(pBits, cx * cx * sizeof(RGBQUAD), 0xFF); /* white, opaque */

    lf = Logfonts[0];
    lf.lfHeight = -(LONG)(cx * 9 / 20);
    lf.lfWidth = 0;
    lf.lfEscapement = lf.lfOrientation = 0;
    lf.lfQuality = ANTIALIASED_QUALITY;
    hFont = CreateFontIndirectW(&lf);

    hbmOld = SelectObject(hdc, hbm);
    hFontOld = SelectObject(hdc, hFont ? hFont : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    SetRect(&rc, 0, 0, cx, cx);
    DrawTextW(hdc, L"Abc", 3, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(hdc, hFontOld);
    SelectObject(hdc, hbmOld);
    GdiFlush();
    if (hFont)
        DeleteObject(hFont);
    DeleteDC(hdc);

    /* Text output leaves the alpha bytes of a 32bpp DIB at zero */
    for (UINT i = 0; i < cx * cx; i++)
        pBits[i].rgbReserved = 0xFF;

    if (pdwAlpha)
        *pdwAlpha = WTSAT_RGB;
    *phbmp = hbm;
    hr = S_OK;

done:
    if (bPrivate)
        RemoveFontResourceExW(m_Path, FR_PRIVATE | FR_NOT_ENUM, NULL);
    return hr;
}
