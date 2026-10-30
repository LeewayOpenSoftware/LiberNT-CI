/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test when a thread keeps the right to take the foreground back
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND hwndFocusOnShow;
static int NcActivate;
static BOOL NcActivateForeground;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_NCACTIVATE && NcActivate < 0)
    {
        NcActivate = wParam ? 1 : 0;
        NcActivateForeground = (GetForegroundWindow() == hwnd);
    }
    if (uMsg == WM_SHOWWINDOW && wParam && hwndFocusOnShow)
        SetFocus(hwndFocusOnShow);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void InjectInput(void)
{
    INPUT input = { 0 };

    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    ok_int(SendInput(1, &input, sizeof(input)), 1);
    PumpMessages();
}

static HWND CreateTestWindow(DWORD Style, HWND *phwndChild)
{
    HWND hwnd;

    hwnd = CreateWindowExW(0, L"ForegroundRightClass", L"ForegroundRight", Style,
                           100, 100, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    *phwndChild = CreateWindowExW(0, L"edit", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  5, 5, 80, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
    return hwnd;
}

static HWND ActivateOnShow(void)
{
    HWND hwnd, hwndChild;

    hwnd = CreateTestWindow(WS_OVERLAPPEDWINDOW, &hwndChild);
    hwndFocusOnShow = hwndChild;
    NcActivate = -1;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    PumpMessages();
    hwndFocusOnShow = NULL;
    return hwnd;
}

START_TEST(ForegroundRight)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd, hwndChild;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ForegroundRightClass";
    RegisterClassW(&wc);

    hwnd = ActivateOnShow();
    if (GetForegroundWindow() != hwnd)
    {
        skip("The test process could not take the foreground\n");
        DestroyWindow(hwnd);
        return;
    }
    ok_int(NcActivate, 1);
    ok_int(NcActivateForeground, TRUE);
    DestroyWindow(hwnd);
    PumpMessages();

    hwnd = CreateTestWindow(WS_OVERLAPPEDWINDOW, &hwndChild);
    NcActivate = -1;
    SetFocus(hwndChild);
    PumpMessages();
    ok_int(NcActivate, 1);
    ok_int(NcActivateForeground, TRUE);
    ok(GetForegroundWindow() == hwnd, "The never shown window is not the foreground window\n");
    DestroyWindow(hwnd);
    PumpMessages();

    hwnd = ActivateOnShow();
    ok_int(NcActivate, 0);
    ok_int(NcActivateForeground, FALSE);
    ok(GetActiveWindow() == hwnd, "The window is not active\n");
    ok(GetForegroundWindow() != hwnd, "The window took the foreground\n");
    DestroyWindow(hwnd);
    PumpMessages();

    hwnd = CreateTestWindow(WS_OVERLAPPEDWINDOW, &hwndChild);
    NcActivate = -1;
    SetActiveWindow(hwnd);
    PumpMessages();
    ok_int(NcActivate, 0);
    ok(GetForegroundWindow() != hwnd, "The window took the foreground\n");
    DestroyWindow(hwnd);
    PumpMessages();

    InjectInput();
    hwnd = ActivateOnShow();
    ok_int(NcActivate, 0);
    ok(GetActiveWindow() == hwnd, "The window is not active\n");
    ok(GetForegroundWindow() != hwnd, "The window took the foreground after injecting input\n");
    DestroyWindow(hwnd);
    PumpMessages();

    UnregisterClassW(L"ForegroundRightClass", GetModuleHandleW(NULL));
}
