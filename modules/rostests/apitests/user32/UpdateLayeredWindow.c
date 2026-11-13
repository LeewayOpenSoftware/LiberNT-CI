/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     UpdateLayeredWindow: a per-pixel-alpha layered window reaches the screen
 */

#include "precomp.h"

/* Everything happens on top of this opaque white window so the screen reads
 * do not depend on the desktop. */
static HWND s_Backdrop;

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void Settle(void)
{
    PumpMessages();
    Sleep(300);
    PumpMessages();
}

static COLORREF ScreenPixel(int x, int y)
{
    HDC hdc = GetDC(NULL);
    COLORREF cr = GetPixel(hdc, x, y);
    ReleaseDC(NULL, hdc);
    return cr;
}

static BOOL NearColor(COLORREF cr, int r, int g, int b, int tolerance)
{
    return abs((int)GetRValue(cr) - r) <= tolerance &&
           abs((int)GetGValue(cr) - g) <= tolerance &&
           abs((int)GetBValue(cr) - b) <= tolerance;
}

/* A 32-bpp top-down DIB selected into a memory DC, premultiplied, filled with
 * one colour. The top-left quadrant gets the alpha QuadrantAlpha. */
static HDC CreateSource(int cx, int cy, BYTE r, BYTE g, BYTE b, BYTE QuadrantAlpha, HBITMAP *phbm)
{
    BITMAPINFO bmi = { 0 };
    DWORD *bits = NULL;
    HDC hdc;
    HBITMAP hbm;
    int x, y;

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    hdc = CreateCompatibleDC(NULL);
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hdc || !hbm)
    {
        if (hbm) DeleteObject(hbm);
        if (hdc) DeleteDC(hdc);
        return NULL;
    }
    for (y = 0; y < cy; y++)
    {
        for (x = 0; x < cx; x++)
        {
            BYTE a = (x < cx / 2 && y < cy / 2) ? QuadrantAlpha : 255;
            bits[y * cx + x] = ((DWORD)a << 24) |
                               ((DWORD)(r * a / 255) << 16) |
                               ((DWORD)(g * a / 255) << 8) |
                               (DWORD)(b * a / 255);
        }
    }
    SelectObject(hdc, hbm);
    *phbm = hbm;
    return hdc;
}

static void DestroySource(HDC hdc, HBITMAP hbm)
{
    if (hdc) DeleteDC(hdc);
    if (hbm) DeleteObject(hbm);
}

static BOOL Ulw(HWND hwnd, HDC hdcSrc, int x, int y, int cx, int cy, BYTE ConstantAlpha)
{
    POINT ptDst = { x, y };
    POINT ptSrc = { 0, 0 };
    SIZE size = { cx, cy };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, ConstantAlpha, AC_SRC_ALPHA };

    SetLastError(0xdeadbeef);
    return UpdateLayeredWindow(hwnd, NULL, &ptDst, &size, hdcSrc, &ptSrc, 0, &blend, ULW_ALPHA);
}

static HWND CreateLayered(void)
{
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           L"UlwTestClass", L"Layered", WS_POPUP,
                           40, 40, 64, 64, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

/* The common pattern: paint the hidden layered window, then show it. */
static void TestUpdateThenShow(void)
{
    HWND hwnd = CreateLayered();
    HBITMAP hbm = NULL;
    HDC hdcSrc;
    COLORREF cr;

    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;

    hdcSrc = CreateSource(64, 64, 255, 0, 0, 255, &hbm);
    ok(hdcSrc != NULL, "could not create the source DIB\n");
    if (hdcSrc)
    {
        ok(Ulw(hwnd, hdcSrc, 40, 40, 64, 64, 255), "UpdateLayeredWindow (hidden) failed, error %lu\n", GetLastError());
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        Settle();
        cr = ScreenPixel(72, 72);
        ok(NearColor(cr, 255, 0, 0, 2), "update-then-show: screen pixel is %#lx, expected red\n", cr);
        cr = ScreenPixel(50, 50);
        ok(NearColor(cr, 255, 0, 0, 2), "update-then-show: top-left pixel is %#lx, expected red\n", cr);
    }
    DestroySource(hdcSrc, hbm);
    DestroyWindow(hwnd);
    Settle();
}

/* Show the empty layered window first, then paint and repaint it. */
static void TestShowThenUpdate(void)
{
    HWND hwnd = CreateLayered();
    HBITMAP hbm = NULL;
    HDC hdcSrc;
    COLORREF cr;

    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Settle();
    cr = ScreenPixel(72, 72);
    ok(NearColor(cr, 255, 255, 255, 2), "show-before-update: screen pixel is %#lx, expected the white backdrop\n", cr);

    hdcSrc = CreateSource(64, 64, 0, 255, 0, 255, &hbm);
    ok(hdcSrc != NULL, "could not create the source DIB\n");
    if (hdcSrc)
    {
        ok(Ulw(hwnd, hdcSrc, 40, 40, 64, 64, 255), "UpdateLayeredWindow (shown) failed, error %lu\n", GetLastError());
        Settle();
        cr = ScreenPixel(72, 72);
        ok(NearColor(cr, 0, 255, 0, 2), "show-then-update: screen pixel is %#lx, expected green\n", cr);
    }
    DestroySource(hdcSrc, hbm);

    /* Repaint in place with another colour. */
    hdcSrc = CreateSource(64, 64, 0, 0, 255, 255, &hbm);
    if (hdcSrc)
    {
        ok(Ulw(hwnd, hdcSrc, 40, 40, 64, 64, 255), "UpdateLayeredWindow (repaint) failed, error %lu\n", GetLastError());
        Settle();
        cr = ScreenPixel(72, 72);
        ok(NearColor(cr, 0, 0, 255, 2), "repaint: screen pixel is %#lx, expected blue\n", cr);
    }
    DestroySource(hdcSrc, hbm);
    DestroyWindow(hwnd);
    Settle();
}

/* Per-pixel alpha: a transparent quadrant shows the backdrop; the constant
 * alpha blends the rest with it. */
static void TestAlpha(void)
{
    HWND hwnd = CreateLayered();
    HBITMAP hbm = NULL;
    HDC hdcSrc;
    COLORREF cr;

    ok(hwnd != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;

    hdcSrc = CreateSource(64, 64, 255, 0, 0, 0, &hbm);
    ok(hdcSrc != NULL, "could not create the source DIB\n");
    if (hdcSrc)
    {
        ok(Ulw(hwnd, hdcSrc, 40, 40, 64, 64, 255), "UpdateLayeredWindow (alpha) failed, error %lu\n", GetLastError());
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        Settle();
        cr = ScreenPixel(50, 50);
        ok(NearColor(cr, 255, 255, 255, 2), "transparent quadrant: screen pixel is %#lx, expected the white backdrop\n", cr);
        cr = ScreenPixel(90, 90);
        ok(NearColor(cr, 255, 0, 0, 2), "opaque part: screen pixel is %#lx, expected red\n", cr);

        ok(Ulw(hwnd, hdcSrc, 40, 40, 64, 64, 128), "UpdateLayeredWindow (constant alpha) failed, error %lu\n", GetLastError());
        Settle();
        cr = ScreenPixel(90, 90);
        ok(NearColor(cr, 255, 127, 127, 6), "half alpha over white: screen pixel is %#lx, expected about (255,127,127)\n", cr);
    }
    DestroySource(hdcSrc, hbm);
    DestroyWindow(hwnd);
    Settle();
}

START_TEST(UpdateLayeredWindow)
{
    WNDCLASSW wc = { 0 };

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"UlwTestClass";
    ok(RegisterClassW(&wc) != 0, "RegisterClassW failed, error %lu\n", GetLastError());

    s_Backdrop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"UlwTestClass", L"Backdrop",
                                 WS_POPUP, 0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(s_Backdrop != NULL, "CreateWindowExW (backdrop) failed, error %lu\n", GetLastError());
    if (!s_Backdrop)
        return;
    ShowWindow(s_Backdrop, SW_SHOWNOACTIVATE);
    UpdateWindow(s_Backdrop);
    Settle();
    ok(NearColor(ScreenPixel(72, 72), 255, 255, 255, 2), "the backdrop did not reach the screen (%#lx); the results below are not meaningful\n",
       ScreenPixel(72, 72));

    TestUpdateThenShow();
    TestShowThenUpdate();
    TestAlpha();

    DestroyWindow(s_Backdrop);
    PumpMessages();
    UnregisterClassW(L"UlwTestClass", GetModuleHandleW(NULL));
}
