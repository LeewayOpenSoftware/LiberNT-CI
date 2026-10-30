/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the painting of a window covered by another window
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND Watched;
static int NcPaints, Erases, Paints;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (hwnd == Watched)
    {
        if (uMsg == WM_NCPAINT)
            NcPaints++;
        else if (uMsg == WM_ERASEBKGND)
            Erases++;
        else if (uMsg == WM_PAINT)
            Paints++;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void CheckPaints(const char *Name, BOOL ExpectedInvalid, int ExpectedNcPaints, int ExpectedErases, int ExpectedPaints)
{
    ok(GetUpdateRect(Watched, NULL, FALSE) == ExpectedInvalid, "%s: the window is %s, expected %s\n",
       Name, ExpectedInvalid ? "valid" : "invalid", ExpectedInvalid ? "invalid" : "valid");
    PumpMessages();
    ok(NcPaints == ExpectedNcPaints, "%s: %d WM_NCPAINT, expected %d\n", Name, NcPaints, ExpectedNcPaints);
    ok(Erases == ExpectedErases, "%s: %d WM_ERASEBKGND, expected %d\n", Name, Erases, ExpectedErases);
    ok(Paints == ExpectedPaints, "%s: %d WM_PAINT, expected %d\n", Name, Paints, ExpectedPaints);
    NcPaints = Erases = Paints = 0;
}

static void TestCovered(void)
{
    HWND Under, Over;
    HDC hdc;
    COLORREF cr;

    Over = CreateWindowExW(WS_EX_TOPMOST, L"CoveredWindowClass", L"Over", WS_POPUP,
                           0, 0, 600, 600, NULL, NULL, GetModuleHandleW(NULL), NULL);
    Under = CreateWindowExW(0, L"CoveredWindowClass", L"Under", WS_POPUP | WS_BORDER,
                            100, 100, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(Under != NULL && Over != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!Under || !Over)
        goto Cleanup;

    SetWindowPos(Over, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    PumpMessages();

    Watched = Under;
    NcPaints = Erases = Paints = 0;
    SetWindowPos(Under, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    CheckPaints("shown under a topmost window", TRUE, 1, 1, 1);

    InvalidateRect(Under, NULL, TRUE);
    CheckPaints("invalidated under a topmost window", TRUE, 0, 1, 1);

    RedrawWindow(Under, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_UPDATENOW);
    CheckPaints("redrawn under a topmost window", FALSE, 1, 1, 1);

    hdc = GetDC(Under);
    cr = SetPixel(hdc, 5, 5, RGB(255, 0, 0));
    ok(cr != CLR_INVALID, "SetPixel failed on the covered window\n");
    ok(GetPixel(hdc, 5, 5) == RGB(255, 0, 0), "GetPixel returned %#lx\n", GetPixel(hdc, 5, 5));
    ReleaseDC(Under, hdc);

    SetWindowPos(Under, NULL, 120, 120, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    CheckPaints("moved under a topmost window", FALSE, 0, 0, 0);

    SetWindowPos(Under, NULL, 0, 0, 150, 150, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    CheckPaints("resized under a topmost window", TRUE, 1, 1, 1);

Cleanup:
    Watched = NULL;
    if (Over)
        DestroyWindow(Over);
    if (Under)
        DestroyWindow(Under);
    PumpMessages();
}

START_TEST(CoveredWindow)
{
    WNDCLASSW wc = { 0 };
    BOOL composition = FALSE;
    HMODULE dwmapi;
    HRESULT (WINAPI *pDwmIsCompositionEnabled)(BOOL *);

    dwmapi = LoadLibraryW(L"dwmapi.dll");
    pDwmIsCompositionEnabled = dwmapi ? (void *)GetProcAddress(dwmapi, "DwmIsCompositionEnabled") : NULL;
    if (!pDwmIsCompositionEnabled || FAILED(pDwmIsCompositionEnabled(&composition)) || !composition)
    {
        skip("Desktop composition is not active\n");
        if (dwmapi)
            FreeLibrary(dwmapi);
        return;
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CoveredWindowClass";
    RegisterClassW(&wc);

    TestCovered();

    UnregisterClassW(L"CoveredWindowClass", GetModuleHandleW(NULL));
    FreeLibrary(dwmapi);
}
