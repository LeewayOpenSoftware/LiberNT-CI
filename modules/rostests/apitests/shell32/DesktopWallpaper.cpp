/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for IDesktopWallpaper
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "shelltest.h"
#include <strsafe.h>

static const WCHAR szDesktopKey[] = L"Control Panel\\Desktop";
static const WCHAR szColorsKey[] = L"Control Panel\\Colors";

typedef struct _SAVED_STATE
{
    WCHAR szWallpaper[MAX_PATH];
    WCHAR szStyle[16];
    WCHAR szTile[16];
    WCHAR szBackground[32];
    COLORREF crDesktop;
} SAVED_STATE;

static void
ReadString(LPCWSTR pszKey, LPCWSTR pszValue, LPWSTR pszBuffer, DWORD cchBuffer)
{
    DWORD cb = cchBuffer * sizeof(WCHAR);
    if (RegGetValueW(HKEY_CURRENT_USER, pszKey, pszValue, RRF_RT_REG_SZ, NULL, pszBuffer, &cb) != ERROR_SUCCESS)
        pszBuffer[0] = UNICODE_NULL;
}

static void
WriteString(LPCWSTR pszKey, LPCWSTR pszValue, LPCWSTR pszData)
{
    HKEY hKey;
    if (!pszData[0] || RegCreateKeyExW(HKEY_CURRENT_USER, pszKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL))
        return;
    RegSetValueExW(hKey, pszValue, 0, REG_SZ, (const BYTE *)pszData, (lstrlenW(pszData) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);
}

static void
SaveState(SAVED_STATE *pState)
{
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(pState->szWallpaper), pState->szWallpaper, 0);
    ReadString(szDesktopKey, L"WallpaperStyle", pState->szStyle, ARRAYSIZE(pState->szStyle));
    ReadString(szDesktopKey, L"TileWallpaper", pState->szTile, ARRAYSIZE(pState->szTile));
    ReadString(szColorsKey, L"Background", pState->szBackground, ARRAYSIZE(pState->szBackground));
    pState->crDesktop = GetSysColor(COLOR_DESKTOP);
}

static void
RestoreState(const SAVED_STATE *pState)
{
    INT iElement = COLOR_DESKTOP;

    WriteString(szDesktopKey, L"WallpaperStyle", pState->szStyle);
    WriteString(szDesktopKey, L"TileWallpaper", pState->szTile);
    WriteString(szColorsKey, L"Background", pState->szBackground);
    SetSysColors(1, &iElement, &pState->crDesktop);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)pState->szWallpaper, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

/* A 2x2 24-bit bitmap the wallpaper loader can decode. */
static BOOL
CreateTestBitmap(LPWSTR pszPath, DWORD cchPath)
{
    BYTE Pixels[2 * 8] = { 0 };
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    WCHAR szTemp[MAX_PATH];
    HANDLE hFile;
    DWORD cbWritten;
    BOOL bOk;

    if (!GetTempPathW(ARRAYSIZE(szTemp), szTemp))
        return FALSE;
    StringCchPrintfW(pszPath, cchPath, L"%sdeskwp_%lu.bmp", szTemp, GetCurrentProcessId());

    ZeroMemory(&bfh, sizeof(bfh));
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = 2;
    bih.biHeight = 2;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biSizeImage = sizeof(Pixels);
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + sizeof(Pixels);

    hFile = CreateFileW(pszPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    bOk = WriteFile(hFile, &bfh, sizeof(bfh), &cbWritten, NULL) &&
          WriteFile(hFile, &bih, sizeof(bih), &cbWritten, NULL) &&
          WriteFile(hFile, Pixels, sizeof(Pixels), &cbWritten, NULL);
    CloseHandle(hFile);
    return bOk;
}

static void
TestMonitors(IDesktopWallpaper *pdw)
{
    UINT cMonitors = 0;
    LPWSTR pszId;
    RECT rc;
    HRESULT hr;

    hr = pdw->GetMonitorDevicePathCount(NULL);
    ok(FAILED(hr), "GetMonitorDevicePathCount(NULL) returned 0x%lx\n", hr);
    hr = pdw->GetMonitorDevicePathAt(0, NULL);
    ok(FAILED(hr), "GetMonitorDevicePathAt(0, NULL) returned 0x%lx\n", hr);

    hr = pdw->GetMonitorDevicePathCount(&cMonitors);
    ok_hr(hr, S_OK);
    ok(cMonitors >= 1, "Expected at least one monitor, got %u\n", cMonitors);

    for (UINT i = 0; i < cMonitors; ++i)
    {
        pszId = NULL;
        hr = pdw->GetMonitorDevicePathAt(i, &pszId);
        ok_hr(hr, S_OK);
        ok(pszId && pszId[0], "Monitor %u has no device path\n", i);
        if (!pszId)
            continue;

        SetRectEmpty(&rc);
        hr = pdw->GetMonitorRECT(pszId, &rc);
        ok_hr(hr, S_OK);
        ok(rc.right > rc.left && rc.bottom > rc.top, "Monitor %u rect is empty (%ld,%ld)-(%ld,%ld)\n",
           i, rc.left, rc.top, rc.right, rc.bottom);
        CoTaskMemFree(pszId);
    }

    pszId = (LPWSTR)(ULONG_PTR)1;
    hr = pdw->GetMonitorDevicePathAt(cMonitors, &pszId);
    ok(FAILED(hr), "GetMonitorDevicePathAt(%u) returned 0x%lx\n", cMonitors, hr);
    ok(pszId == NULL, "Expected NULL, got %p\n", pszId);

    hr = pdw->GetMonitorRECT(L"\\\\?\\NoSuchMonitor#0", &rc);
    ok(FAILED(hr), "GetMonitorRECT(unknown) returned 0x%lx\n", hr);
}

static void
TestWallpaper(IDesktopWallpaper *pdw, LPCWSTR pszBitmap)
{
    WCHAR szSpi[MAX_PATH], szReg[MAX_PATH], szBefore[MAX_PATH];
    LPWSTR pszWallpaper, pszFirstMonitor = NULL;
    HRESULT hr;

    hr = pdw->GetWallpaper(NULL, NULL);
    ok(FAILED(hr), "GetWallpaper(NULL, NULL) returned 0x%lx\n", hr);
    hr = pdw->SetWallpaper(NULL, NULL);
    ok(FAILED(hr), "SetWallpaper(NULL, NULL) returned 0x%lx\n", hr);

    /* A missing file must fail and leave the wallpaper as it was. */
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szBefore), szBefore, 0);
    hr = pdw->SetWallpaper(NULL, L"C:\\DoesNotExist\\NoSuchWallpaper.bmp");
    ok(FAILED(hr), "SetWallpaper(missing file) returned 0x%lx\n", hr);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(!lstrcmpiW(szSpi, szBefore), "Wallpaper changed to '%ls'\n", szSpi);

    hr = pdw->SetWallpaper(NULL, pszBitmap);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(!lstrcmpiW(szSpi, pszBitmap), "SPI wallpaper is '%ls', expected '%ls'\n", szSpi, pszBitmap);
    ReadString(szDesktopKey, L"Wallpaper", szReg, ARRAYSIZE(szReg));
    ok(!lstrcmpiW(szReg, pszBitmap), "Registry wallpaper is '%ls', expected '%ls'\n", szReg, pszBitmap);

    pszWallpaper = NULL;
    hr = pdw->GetWallpaper(NULL, &pszWallpaper);
    ok_hr(hr, S_OK);
    ok(pszWallpaper && !lstrcmpiW(pszWallpaper, pszBitmap), "GetWallpaper returned '%ls'\n", pszWallpaper);
    CoTaskMemFree(pszWallpaper);

    if (SUCCEEDED(pdw->GetMonitorDevicePathAt(0, &pszFirstMonitor)))
    {
        pszWallpaper = NULL;
        hr = pdw->GetWallpaper(pszFirstMonitor, &pszWallpaper);
        ok_hr(hr, S_OK);
        ok(pszWallpaper && !lstrcmpiW(pszWallpaper, pszBitmap), "GetWallpaper(monitor 0) returned '%ls'\n", pszWallpaper);
        CoTaskMemFree(pszWallpaper);
        CoTaskMemFree(pszFirstMonitor);
    }

    hr = pdw->SetWallpaper(L"\\\\?\\NoSuchMonitor#0", pszBitmap);
    ok(FAILED(hr), "SetWallpaper(unknown monitor) returned 0x%lx\n", hr);

    /* Disabling empties the live wallpaper; enabling restores the saved one. */
    hr = pdw->Enable(FALSE);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(szSpi[0] == UNICODE_NULL, "Wallpaper still '%ls' after Enable(FALSE)\n", szSpi);
    ReadString(szDesktopKey, L"Wallpaper", szReg, ARRAYSIZE(szReg));
    ok(!lstrcmpiW(szReg, pszBitmap), "Enable(FALSE) changed the saved wallpaper to '%ls'\n", szReg);
    hr = pdw->Enable(TRUE);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(!lstrcmpiW(szSpi, pszBitmap), "Wallpaper is '%ls' after Enable(TRUE)\n", szSpi);
}

static void
TestPosition(IDesktopWallpaper *pdw)
{
    static const DESKTOP_WALLPAPER_POSITION Positions[] =
    {
        DWPOS_CENTER, DWPOS_TILE, DWPOS_STRETCH, DWPOS_FIT, DWPOS_FILL
    };
    DESKTOP_WALLPAPER_POSITION Position;
    HRESULT hr;

    hr = pdw->GetPosition(NULL);
    ok(FAILED(hr), "GetPosition(NULL) returned 0x%lx\n", hr);

    for (UINT i = 0; i < ARRAYSIZE(Positions); ++i)
    {
        hr = pdw->SetPosition(Positions[i]);
        ok(hr == S_OK, "SetPosition(%d) returned 0x%lx\n", Positions[i], hr);
        Position = (DESKTOP_WALLPAPER_POSITION)-1;
        hr = pdw->GetPosition(&Position);
        ok_hr(hr, S_OK);
        ok(Position == Positions[i], "GetPosition returned %d, expected %d\n", Position, Positions[i]);
    }

    /* Span is optional until win32k gains a span mode. */
    hr = pdw->SetPosition(DWPOS_SPAN);
    ok(hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "SetPosition(DWPOS_SPAN) returned 0x%lx\n", hr);

    hr = pdw->SetPosition((DESKTOP_WALLPAPER_POSITION)(DWPOS_SPAN + 1));
    ok(FAILED(hr), "SetPosition(invalid) returned 0x%lx\n", hr);
}

static void
TestBackgroundColor(IDesktopWallpaper *pdw)
{
    COLORREF cr = 0;
    HRESULT hr;

    hr = pdw->GetBackgroundColor(NULL);
    ok(FAILED(hr), "GetBackgroundColor(NULL) returned 0x%lx\n", hr);

    hr = pdw->SetBackgroundColor(RGB(0x12, 0x34, 0x56));
    ok_hr(hr, S_OK);
    hr = pdw->GetBackgroundColor(&cr);
    ok_hr(hr, S_OK);
    ok(cr == RGB(0x12, 0x34, 0x56), "GetBackgroundColor returned 0x%06lx\n", cr);
    ok(GetSysColor(COLOR_DESKTOP) == RGB(0x12, 0x34, 0x56), "COLOR_DESKTOP is 0x%06lx\n", GetSysColor(COLOR_DESKTOP));
}

static void
TestSlideshow(IDesktopWallpaper *pdw)
{
    DESKTOP_SLIDESHOW_STATE State = (DESKTOP_SLIDESHOW_STATE)-1;
    HRESULT hr;

    hr = pdw->GetStatus(NULL);
    ok(FAILED(hr), "GetStatus(NULL) returned 0x%lx\n", hr);
    hr = pdw->GetStatus(&State);
    ok_hr(hr, S_OK);
    ok(!(State & DSS_SLIDESHOW), "No slideshow was configured, state 0x%x\n", State);
}

START_TEST(DesktopWallpaper)
{
    CComPtr<IDesktopWallpaper> pdw;
    SAVED_STATE Saved;
    WCHAR szBitmap[MAX_PATH];
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok_hr(hr, S_OK);

    hr = CoCreateInstance(CLSID_DesktopWallpaper, NULL, CLSCTX_INPROC_SERVER, IID_IDesktopWallpaper, reinterpret_cast<void **>(&pdw));
    if (FAILED(hr) && GetNTVersion() < _WIN32_WINNT_WIN8)
    {
        skip("IDesktopWallpaper needs Windows 8 or later (0x%lx)\n", hr);
        CoUninitialize();
        return;
    }
    ok_hr(hr, S_OK);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }

    SaveState(&Saved);

    TestMonitors(pdw);
    if (CreateTestBitmap(szBitmap, ARRAYSIZE(szBitmap)))
    {
        TestWallpaper(pdw, szBitmap);
        TestPosition(pdw);
    }
    else
    {
        skip("Cannot create the test bitmap (%lu)\n", GetLastError());
        szBitmap[0] = UNICODE_NULL;
    }
    TestBackgroundColor(pdw);
    TestSlideshow(pdw);

    RestoreState(&Saved);
    if (szBitmap[0])
        DeleteFileW(szBitmap);

    pdw.Release();
    CoUninitialize();
}
