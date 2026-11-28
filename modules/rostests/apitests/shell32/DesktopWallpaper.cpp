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
    RegSetValueExW(hKey, pszValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(pszData), (lstrlenW(pszData) + 1) * sizeof(WCHAR));
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

    LPWSTR Ids[16] = { NULL };
    for (UINT i = 0; i < cMonitors; ++i)
    {
        pszId = NULL;
        hr = pdw->GetMonitorDevicePathAt(i, &pszId);
        ok_hr(hr, S_OK);
        ok(pszId && pszId[0], "Monitor %u has no device path\n", i);
        if (!pszId)
            continue;

        /* IDs are opaque but must be unique and round-trip through GetMonitorRECT. */
        for (UINT j = 0; j < i && j < ARRAYSIZE(Ids); ++j)
            ok(!Ids[j] || lstrcmpiW(Ids[j], pszId), "Monitors %u and %u share the ID '%ls'\n", j, i, pszId);

        SetRectEmpty(&rc);
        hr = pdw->GetMonitorRECT(pszId, &rc);
        ok_hr(hr, S_OK);
        ok(rc.right > rc.left && rc.bottom > rc.top, "Monitor %u rect is empty (%ld,%ld)-(%ld,%ld)\n",
           i, rc.left, rc.top, rc.right, rc.bottom);

        if (i < ARRAYSIZE(Ids))
            Ids[i] = pszId;
        else
            CoTaskMemFree(pszId);
    }
    for (UINT i = 0; i < ARRAYSIZE(Ids); ++i)
        CoTaskMemFree(Ids[i]);

    pszId = (LPWSTR)(ULONG_PTR)1;
    hr = pdw->GetMonitorDevicePathAt(cMonitors, &pszId);
    ok_hr(hr, E_FAIL);
    ok(pszId == NULL, "Expected NULL, got %p\n", pszId);

    hr = pdw->GetMonitorRECT(L"\\\\?\\NoSuchMonitor#0", &rc);
    ok(FAILED(hr), "GetMonitorRECT(unknown) returned 0x%lx\n", hr);
}

/* Win11 commits SetWallpaper to SPI, the registry and the monitor asynchronously (within 1 s). */
static BOOL
WaitForWallpaper(IDesktopWallpaper *pdw, LPCWSTR pszMonitor, LPCWSTR pszBitmap,
                 LPWSTR pszSpi, LPWSTR pszReg, DWORD cch, LPWSTR *ppszMonitor)
{
    for (DWORD dwWaited = 0;; dwWaited += 50)
    {
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, cch, pszSpi, 0);
        ReadString(szDesktopKey, L"Wallpaper", pszReg, cch);
        *ppszMonitor = NULL;
        if (pszMonitor && FAILED(pdw->GetWallpaper(pszMonitor, ppszMonitor)))
            *ppszMonitor = NULL;
        if (!lstrcmpiW(pszSpi, pszBitmap) && !lstrcmpiW(pszReg, pszBitmap) &&
            (!pszMonitor || (*ppszMonitor && !lstrcmpiW(*ppszMonitor, pszBitmap))))
            return TRUE;
        if (dwWaited >= 5000)
            return FALSE;
        CoTaskMemFree(*ppszMonitor);
        Sleep(50);
    }
}

static void
ExpectState(IDesktopWallpaper *pdw, LPCWSTR pszBitmap, BOOL bEnabled, LPCSTR pszWhen)
{
    DESKTOP_SLIDESHOW_STATE State = (DESKTOP_SLIDESHOW_STATE)-1;
    LPWSTR pszWallpaper = NULL;
    HRESULT hr;

    hr = pdw->GetStatus(&State);
    ok_hr(hr, S_OK);
    ok(State == (bEnabled ? DSS_ENABLED : 0), "%s: state 0x%x\n", pszWhen, State);
    hr = pdw->GetWallpaper(NULL, &pszWallpaper);
    ok_hr(hr, S_OK);
    /* While disabled, Win11 has answered both the chosen path and an empty string. */
    ok(pszWallpaper && (!lstrcmpiW(pszWallpaper, pszBitmap) || (!bEnabled && !pszWallpaper[0])),
       "%s: GetWallpaper returned '%ls'\n", pszWhen, pszWallpaper);
    CoTaskMemFree(pszWallpaper);
}

/* Win11: Enable(FALSE) empties the live and the saved wallpaper and persists. */
static void
TestDisable(IDesktopWallpaper *pdw, LPCWSTR pszBitmap)
{
    CComPtr<IDesktopWallpaper> pOther;
    WCHAR szSpi[MAX_PATH], szReg[MAX_PATH];
    HRESULT hr;

    ExpectState(pdw, pszBitmap, TRUE, "enabled");

    hr = pdw->Enable(FALSE);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(szSpi[0] == UNICODE_NULL, "Wallpaper still '%ls' after Enable(FALSE)\n", szSpi);
    ReadString(szDesktopKey, L"Wallpaper", szReg, ARRAYSIZE(szReg));
    ok(szReg[0] == UNICODE_NULL, "Enable(FALSE) left the saved wallpaper '%ls'\n", szReg);
    ExpectState(pdw, pszBitmap, FALSE, "disabled");

    hr = CoCreateInstance(CLSID_DesktopWallpaper, NULL, CLSCTX_ALL, IID_IDesktopWallpaper, reinterpret_cast<void **>(&pOther));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
        ExpectState(pOther, pszBitmap, FALSE, "disabled, new instance");

    /* Changing the position does not bring the wallpaper back. */
    hr = pdw->SetPosition(DWPOS_FILL);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(szSpi[0] == UNICODE_NULL, "SetPosition re-enabled the wallpaper '%ls'\n", szSpi);
    ExpectState(pdw, pszBitmap, FALSE, "disabled, after SetPosition");

    /* Not in the dwprobe run: SetWallpaper while disabled keeps the desktop blank. */
    hr = pdw->SetWallpaper(NULL, pszBitmap);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(szSpi[0] == UNICODE_NULL, "SetWallpaper re-enabled the wallpaper '%ls'\n", szSpi);
    ExpectState(pdw, pszBitmap, FALSE, "disabled, after SetWallpaper");

    hr = pdw->Enable(TRUE);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    /* Win11 shows a transcoded copy; Orosi applies the file itself. */
    if (is_reactos())
        ok(!lstrcmpiW(szSpi, pszBitmap), "Wallpaper is '%ls' after Enable(TRUE)\n", szSpi);
    else
        ok(szSpi[0] != UNICODE_NULL, "No wallpaper after Enable(TRUE)\n");
    ExpectState(pdw, pszBitmap, TRUE, "enabled again");

    /* A wallpaper set behind the interface's back ends the disabled state. */
    hr = pdw->Enable(FALSE);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)pszBitmap, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    ExpectState(pdw, pszBitmap, TRUE, "disabled, then SPI_SETDESKWALLPAPER");
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
    ok_hr(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(!lstrcmpiW(szSpi, szBefore), "Wallpaper changed to '%ls'\n", szSpi);

    if (FAILED(pdw->GetMonitorDevicePathAt(0, &pszFirstMonitor)))
        pszFirstMonitor = NULL;

    hr = pdw->SetWallpaper(NULL, pszBitmap);
    ok_hr(hr, S_OK);
    /* The interface itself reports the new path at once. */
    pszWallpaper = NULL;
    hr = pdw->GetWallpaper(NULL, &pszWallpaper);
    ok_hr(hr, S_OK);
    ok(pszWallpaper && !lstrcmpiW(pszWallpaper, pszBitmap), "GetWallpaper returned '%ls'\n", pszWallpaper);
    CoTaskMemFree(pszWallpaper);

    pszWallpaper = NULL;
    ok(WaitForWallpaper(pdw, pszFirstMonitor, pszBitmap, szSpi, szReg, ARRAYSIZE(szSpi), &pszWallpaper),
       "Wallpaper not committed: SPI '%ls', registry '%ls', monitor 0 '%ls', expected '%ls'\n",
       szSpi, szReg, pszWallpaper, pszBitmap);
    CoTaskMemFree(pszWallpaper);

    /* Win11 checks the file before the monitor, then accepts an unknown monitor and changes nothing. */
    hr = pdw->SetWallpaper(L"\\\\?\\NoSuchMonitor#0", L"C:\\DoesNotExist\\NoSuchWallpaper.bmp");
    ok_hr(hr, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    hr = pdw->SetWallpaper(L"\\\\?\\NoSuchMonitor#0", pszBitmap);
    ok_hr(hr, S_OK);
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(szSpi), szSpi, 0);
    ok(!lstrcmpiW(szSpi, pszBitmap), "SetWallpaper(unknown monitor) changed the wallpaper to '%ls'\n", szSpi);

    /* Win11 briefly answers S_FALSE for every monitor after that call; start the disable test settled. */
    hr = pdw->SetWallpaper(NULL, pszBitmap);
    ok_hr(hr, S_OK);
    pszWallpaper = NULL;
    WaitForWallpaper(pdw, pszFirstMonitor, pszBitmap, szSpi, szReg, ARRAYSIZE(szSpi), &pszWallpaper);
    CoTaskMemFree(pszWallpaper);
    CoTaskMemFree(pszFirstMonitor);

    TestDisable(pdw, pszBitmap);
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

    /* Orosi's win32k has no span mode yet. */
    hr = pdw->SetPosition(DWPOS_SPAN);
    if (is_reactos())
        ok_hr(hr, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    else
        ok_hr(hr, S_OK);

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
    /* Win11 left COLOR_DESKTOP unchanged in the parity run. */
    if (is_reactos())
        ok(GetSysColor(COLOR_DESKTOP) == RGB(0x12, 0x34, 0x56), "COLOR_DESKTOP is 0x%06lx\n", GetSysColor(COLOR_DESKTOP));

    hr = pdw->SetBackgroundColor(0x01000000);
    ok_hr(hr, S_OK);
}

static void
TestSlideshow(IDesktopWallpaper *pdw)
{
    DESKTOP_SLIDESHOW_STATE State = (DESKTOP_SLIDESHOW_STATE)-1;
    DESKTOP_SLIDESHOW_OPTIONS Options = (DESKTOP_SLIDESHOW_OPTIONS)-1;
    IShellItemArray *pItems = NULL;
    UINT uTick = (UINT)-1;
    HRESULT hr;

    hr = pdw->GetStatus(NULL);
    ok(FAILED(hr), "GetStatus(NULL) returned 0x%lx\n", hr);
    hr = pdw->GetStatus(&State);
    ok_hr(hr, S_OK);
    ok(State == DSS_ENABLED, "No slideshow was configured, state 0x%x\n", State);

    /* With no slideshow configured (Win11 parity run values). */
    hr = pdw->GetSlideshow(&pItems);
    ok_hr(hr, S_FALSE);
    if (pItems)
        pItems->Release();
    hr = pdw->GetSlideshowOptions(&Options, &uTick);
    ok_hr(hr, S_OK);
    ok(Options == 0 && uTick == 1800000, "GetSlideshowOptions returned 0x%x, %u\n", Options, uTick);
    hr = pdw->AdvanceSlideshow(NULL, DSD_FORWARD);
    ok_hr(hr, E_UNEXPECTED);
}

START_TEST(DesktopWallpaper)
{
    CComPtr<IDesktopWallpaper> pdw;
    SAVED_STATE Saved;
    WCHAR szBitmap[MAX_PATH];
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok_hr(hr, S_OK);

    hr = CoCreateInstance(CLSID_DesktopWallpaper, NULL, CLSCTX_ALL, IID_IDesktopWallpaper, reinterpret_cast<void **>(&pdw));
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
