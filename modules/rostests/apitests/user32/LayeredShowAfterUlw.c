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

    // Create hidden WS_EX_LAYERED popup window
    hwnd = CreateWindowExW(
        WS_EX_LAYERED,              // Layered window
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

    // Fill DIB with solid red color (ARGB: 0xFF0000FF)
    for (i = 0; i < (DWORD)(szWindow.cx * szWindow.cy); i++) {
        pBits[i] = 0xFF0000FF;  // Red, fully opaque
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
    UpdateWindow(hwnd);
    ok(IsWindowVisible(hwnd), "Window should be visible after ShowWindow\n");

    // Give rendering time
    Sleep(100);

    // Read pixel from screen to verify surface was painted
    hdcScreen = GetDC(NULL);
    ok(hdcScreen != NULL, "GetDC(NULL) failed\n");

    if (hdcScreen) {
        // Read center pixel (50+50, 50+50) = (100, 100)
        pixelColor = GetPixel(hdcScreen, 100, 100);
        // Color should be red (0xFF0000) or close to it
        // Allow some variance for color conversion
        ok((pixelColor & 0xFF0000) == 0xFF0000 || pixelColor == 0xFF0000FF,
           "Screen pixel should be red after ShowWindow; got 0x%lX\n", pixelColor);

        ReleaseDC(NULL, hdcScreen);
    }

    // Test control: window shown BEFORE ULW (should always work)
    DestroyWindow(hwnd);
    DeleteDC(hdc);
    DeleteObject(hbm);

    // Create visible layered window first
    hwnd = CreateWindowExW(
        WS_EX_LAYERED,
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

    // Fill with green
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

    Sleep(100);

    hdcScreen = GetDC(NULL);
    ok(hdcScreen != NULL, "GetDC(NULL) failed for control\n");

    if (hdcScreen) {
        pixelColor = GetPixel(hdcScreen, 100, 100);
        ok((pixelColor & 0xFF00FF00) == 0xFF00FF00 || pixelColor == 0xFF00FF00,
           "Control: Screen pixel should be green; got 0x%lX\n", pixelColor);

        ReleaseDC(NULL, hdcScreen);
    }

    DestroyWindow(hwnd);
    DeleteDC(hdc);
    DeleteObject(hbm);
}

START_TEST(LayeredShowAfterUlw)
{
    Test_HiddenLayeredWindowULWShowWindow();
}
