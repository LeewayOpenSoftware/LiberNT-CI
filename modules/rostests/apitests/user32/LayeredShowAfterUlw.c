/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test UpdateLayeredWindow on hidden window followed by ShowWindow
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static void Test_HiddenLayeredWindowULWShowWindow(void)
{
    HWND hwnd;
    HDC hdc, hdcScreen;
    HBITMAP hbm;
    BITMAPINFOHEADER bih;
    BLENDFUNCTION bf;
    SIZE szWindow = { 100, 100 };
    POINT ptSrc = { 0, 0 };
    POINT ptDst = { 50, 50 };
    DWORD *pBits = NULL;
    DWORD i;
    COLORREF pixelColor;
    BOOL ulwResult;

    // Create hidden WS_EX_LAYERED popup window with TOPMOST to ensure visibility
    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"STATIC",
        L"",
        WS_POPUP,                   // NO WS_VISIBLE
        50, 50, 100, 100,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    ok(hwnd != NULL, "CreateWindowEx failed\n");
    if (!hwnd) return;

    // Verify window is not visible
    ok(!IsWindowVisible(hwnd), "Window should not be visible initially\n");

    // Create 32-bit DIB for surface
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = szWindow.cx;
    bih.biHeight = szWindow.cy;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    hbm = CreateDIBSection(NULL, (BITMAPINFO *)&bih, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    ok(hbm != NULL, "CreateDIBSection failed\n");
    if (!hbm) {
        DestroyWindow(hwnd);
        return;
    }

    // Fill DIB with solid red color (ARGB: 0xFFFF0000 = A=255, R=255, G=0, B=0)
    for (i = 0; i < (DWORD)(szWindow.cx * szWindow.cy); i++) {
        pBits[i] = 0xFFFF0000;  // Red, fully opaque
    }

    // Create DC and select DIB
    hdc = CreateCompatibleDC(NULL);
    ok(hdc != NULL, "CreateCompatibleDC failed\n");
    if (!hdc) {
        DeleteObject(hbm);
        DestroyWindow(hwnd);
        return;
    }

    SelectObject(hdc, hbm);

    // Set blend function (per-pixel alpha)
    ZeroMemory(&bf, sizeof(bf));
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;  // Opaque
    bf.AlphaFormat = AC_SRC_ALPHA;

    // Call UpdateLayeredWindow on hidden window
    ulwResult = UpdateLayeredWindow(hwnd, NULL, &ptDst, &szWindow, hdc, &ptSrc, 0, &bf, ULW_ALPHA);
    ok(ulwResult, "UpdateLayeredWindow failed on hidden window\n");

    // Verify window is still not visible
    ok(!IsWindowVisible(hwnd), "Window should still be hidden after ULW\n");

    // Now make window visible
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    ok(IsWindowVisible(hwnd), "Window should be visible after ShowWindow\n");

    // Give rendering time (DWM async composition on Windows 11)
    Sleep(200);

    // Read pixel from screen to verify surface was painted
    hdcScreen = GetDC(NULL);
    ok(hdcScreen != NULL, "GetDC(NULL) failed\n");

    if (hdcScreen) {
        // Get window center for sampling (not fixed coordinates)
        RECT rcWnd;
        GetWindowRect(hwnd, &rcWnd);
        int sampleX = rcWnd.left + (rcWnd.right - rcWnd.left) / 2;
        int sampleY = rcWnd.top + (rcWnd.bottom - rcWnd.top) / 2;

        pixelColor = GetPixel(hdcScreen, sampleX, sampleY);
        // COLORREF is 0x00BBGGRR; red (0xFFFF0000 in DIB) = 0x000000FF
        ok(pixelColor == 0x000000FF,
           "Screen pixel should be red (0x000000FF) after ShowWindow; got 0x%08lX\n", pixelColor);

        ReleaseDC(NULL, hdcScreen);
    }

    // Test control: window shown BEFORE ULW (should always work)
    DestroyWindow(hwnd);
    DeleteDC(hdc);
    DeleteObject(hbm);

    // Create visible layered window first with TOPMOST
    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"STATIC",
        L"",
        WS_POPUP | WS_VISIBLE,  // Visible from start
        50, 50, 100, 100,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    ok(hwnd != NULL, "CreateWindowEx for control failed\n");
    if (!hwnd) return;

    ok(IsWindowVisible(hwnd), "Control window should be visible\n");

    // Create new DIB
    hbm = CreateDIBSection(NULL, (BITMAPINFO *)&bih, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    ok(hbm != NULL, "CreateDIBSection for control failed\n");
    if (!hbm) {
        DestroyWindow(hwnd);
        return;
    }

    // Fill with green (ARGB: 0xFF00FF00 = A=255, R=0, G=255, B=0)
    for (i = 0; i < (DWORD)(szWindow.cx * szWindow.cy); i++) {
        pBits[i] = 0xFF00FF00;  // Green, fully opaque
    }

    hdc = CreateCompatibleDC(NULL);
    ok(hdc != NULL, "CreateCompatibleDC for control failed\n");
    if (!hdc) {
        DeleteObject(hbm);
        DestroyWindow(hwnd);
        return;
    }

    SelectObject(hdc, hbm);

    // ULW on visible window (control)
    ulwResult = UpdateLayeredWindow(hwnd, NULL, &ptDst, &szWindow, hdc, &ptSrc, 0, &bf, ULW_ALPHA);
    ok(ulwResult, "UpdateLayeredWindow failed on visible window\n");

    Sleep(200);

    hdcScreen = GetDC(NULL);
    ok(hdcScreen != NULL, "GetDC(NULL) failed for control\n");

    if (hdcScreen) {
        // Get window center for sampling
        RECT rcWnd;
        GetWindowRect(hwnd, &rcWnd);
        int sampleX = rcWnd.left + (rcWnd.right - rcWnd.left) / 2;
        int sampleY = rcWnd.top + (rcWnd.bottom - rcWnd.top) / 2;

        pixelColor = GetPixel(hdcScreen, sampleX, sampleY);
        // COLORREF is 0x00BBGGRR; green (0xFF00FF00 in DIB) = 0x0000FF00
        ok(pixelColor == 0x0000FF00,
           "Control: Screen pixel should be green (0x0000FF00); got 0x%08lX\n", pixelColor);

        ReleaseDC(NULL, hdcScreen);
    }

    DestroyWindow(hwnd);
    DeleteDC(hdc);
    DeleteObject(hbm);
}

START_TEST(LayeredShowAfterUlw)
{
    if (!IsWindowVisible(GetDesktopWindow()))
    {
        skip("Not on the interactive desktop\n");
        return;
    }

    Test_HiddenLayeredWindowULWShowWindow();
}
