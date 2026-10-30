/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the repaint of a window uncovered by another window
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

static void StartWatch(const char *Name, HWND hwnd)
{
    PumpMessages();
    ok(!GetUpdateRect(hwnd, NULL, FALSE), "%s: the window is still invalid before the change\n", Name);
    Watched = hwnd;
    NcPaints = Erases = Paints = 0;
}

static void CheckWatch(const char *Name, BOOL ExpectedInvalid, int ExpectedNcPaints, int ExpectedErases, int ExpectedPaints)
{
    ok(GetUpdateRect(Watched, NULL, FALSE) == ExpectedInvalid, "%s: the window is %s, expected %s\n",
       Name, ExpectedInvalid ? "valid" : "invalid", ExpectedInvalid ? "invalid" : "valid");
    PumpMessages();
    ok(NcPaints == ExpectedNcPaints, "%s: %d WM_NCPAINT, expected %d\n", Name, NcPaints, ExpectedNcPaints);
    ok(Erases == ExpectedErases, "%s: %d WM_ERASEBKGND, expected %d\n", Name, Erases, ExpectedErases);
    ok(Paints == ExpectedPaints, "%s: %d WM_PAINT, expected %d\n", Name, Paints, ExpectedPaints);
    Watched = NULL;
}

static void TestTopLevel(void)
{
    HWND Under, Over;

    Under = CreateWindowExW(0, L"UncoveredWindowClass", L"Under", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 300, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    Over = CreateWindowExW(0, L"UncoveredWindowClass", L"Over", WS_POPUP | WS_BORDER,
                           150, 150, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(Under != NULL && Over != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!Under || !Over)
        goto Cleanup;

    SetWindowPos(Over, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    StartWatch("hidden popup", Under);
    SetWindowPos(Over, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    CheckWatch("hidden popup", FALSE, 0, 0, 0);

    SetWindowPos(Over, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    StartWatch("moved popup", Under);
    SetWindowPos(Over, NULL, 600, 600, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    CheckWatch("moved popup", FALSE, 0, 0, 0);

    SetWindowPos(Over, HWND_TOP, 150, 150, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    StartWatch("popup moved below", Under);
    SetWindowPos(Over, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    CheckWatch("popup moved below", FALSE, 0, 0, 0);

Cleanup:
    if (Over)
        DestroyWindow(Over);
    if (Under)
        DestroyWindow(Under);
    PumpMessages();
}

static void TestChild(void)
{
    HWND Parent, Under, Over;

    Parent = CreateWindowExW(0, L"UncoveredWindowClass", L"Parent", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                             100, 100, 300, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(Parent != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (!Parent)
        return;
    Under = CreateWindowExW(0, L"UncoveredWindowClass", L"Under", WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER | WS_VISIBLE,
                            10, 10, 150, 150, Parent, NULL, GetModuleHandleW(NULL), NULL);
    Over = CreateWindowExW(0, L"UncoveredWindowClass", L"Over", WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER,
                           50, 50, 150, 150, Parent, NULL, GetModuleHandleW(NULL), NULL);
    ok(Under != NULL && Over != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    if (Under && Over)
    {
        SetWindowPos(Over, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        StartWatch("hidden sibling", Under);
        SetWindowPos(Over, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        CheckWatch("hidden sibling", TRUE, 1, 1, 1);
    }
    DestroyWindow(Parent);
    PumpMessages();
}

START_TEST(UncoveredWindow)
{
    WNDCLASSW wc = { 0 };
    BOOL composition = FALSE;
    HMODULE dwmapi;
    HRESULT (WINAPI *pDwmIsCompositionEnabled)(BOOL *);

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"UncoveredWindowClass";
    RegisterClassW(&wc);

    TestChild();

    dwmapi = LoadLibraryW(L"dwmapi.dll");
    pDwmIsCompositionEnabled = dwmapi ? (void *)GetProcAddress(dwmapi, "DwmIsCompositionEnabled") : NULL;
    if (!pDwmIsCompositionEnabled || FAILED(pDwmIsCompositionEnabled(&composition)) || !composition)
        skip("Desktop composition is not active\n");
    else
        TestTopLevel();

    if (dwmapi)
        FreeLibrary(dwmapi);
    UnregisterClassW(L"UncoveredWindowClass", GetModuleHandleW(NULL));
}
