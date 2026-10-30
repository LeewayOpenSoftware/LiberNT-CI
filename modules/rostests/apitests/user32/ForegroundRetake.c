/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test when a thread takes the foreground back from another thread of its process
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND HelperWindow;
static int NcActivate;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_NCACTIVATE && NcActivate < 0)
        NcActivate = wParam ? 1 : 0;
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return FALSE;
}

static LRESULT CALLBACK HelperProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_USER)
        return SetForegroundWindow((HWND)wParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void WaitForForeground(void)
{
    int i;

    PumpMessages();
    for (i = 0; i < 100 && !GetForegroundWindow(); i++)
    {
        Sleep(10);
        PumpMessages();
    }
}

static DWORD WINAPI HelperThread(LPVOID Parameter)
{
    MSG msg;

    HelperWindow = CreateWindowExW(0, L"static", NULL, WS_POPUP | WS_VISIBLE, 100, 100, 5, 5,
                                   NULL, NULL, NULL, NULL);
    SetWindowLongPtrW(HelperWindow, GWLP_WNDPROC, (LONG_PTR)HelperProc);
    PumpMessages();
    SetEvent((HANDLE)Parameter);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

static void GiveForeground(HWND hwnd)
{
    SendMessageW(HelperWindow, WM_USER, (WPARAM)hwnd, 0);
    PumpMessages();
}

static HWND CreateTestWindow(BOOL Visible)
{
    HWND hwnd;

    hwnd = CreateWindowExW(0, L"ForegroundRetakeClass", L"ForegroundRetake",
                           WS_OVERLAPPEDWINDOW | (Visible ? WS_VISIBLE : 0), 100, 100, 200, 200,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    PumpMessages();
    return hwnd;
}

static void ConsumeStartupRight(void)
{
    HWND hwnd = CreateTestWindow(TRUE);

    GiveForeground(HelperWindow);
    DestroyWindow(hwnd);
    PumpMessages();
}

static DWORD WINAPI RestoreThread(LPVOID Parameter)
{
    HWND hwnd;

    ConsumeStartupRight();
    hwnd = CreateTestWindow(TRUE);
    ok(GetActiveWindow() == hwnd, "The window is not active\n");
    ok(GetForegroundWindow() == HelperWindow, "The window took the foreground\n");
    ShowWindow(hwnd, SW_MINIMIZE);
    PumpMessages();
    NcActivate = -1;
    ShowWindow(hwnd, SW_RESTORE);
    PumpMessages();
    ok(GetForegroundWindow() == hwnd, "The restored window is not the foreground window\n");
    ok_int(NcActivate, 1);

    ShowWindow(hwnd, SW_MINIMIZE);
    PumpMessages();
    GiveForeground(HelperWindow);
    NcActivate = -1;
    DefWindowProcW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    PumpMessages();
    ok(GetForegroundWindow() == hwnd, "SC_RESTORE did not make the window foreground\n");
    ok_int(NcActivate, 1);
    DestroyWindow(hwnd);
    PumpMessages();
    return 0;
}

static DWORD WINAPI HiddenDialogThread(LPVOID Parameter)
{
    static struct { DLGTEMPLATE Template; WORD Menu, Class, Title; } Dialog;
    HWND hdlg, hwnd;

    ConsumeStartupRight();
    Dialog.Template.style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    Dialog.Template.cx = 100;
    Dialog.Template.cy = 50;
    hdlg = CreateDialogIndirectParamW(GetModuleHandleW(NULL), &Dialog.Template, NULL, DialogProc, 0);
    ok(hdlg != NULL, "CreateDialogIndirectParamW failed, error %lu\n", GetLastError());
    GiveForeground(hdlg);
    ok(GetForegroundWindow() == hdlg, "The hidden dialog is not the foreground window\n");
    EndDialog(hdlg, 0);
    DestroyWindow(hdlg);
    WaitForForeground();
    ok(GetForegroundWindow() == HelperWindow, "The helper window is not the foreground window\n");

    NcActivate = -1;
    hwnd = CreateTestWindow(TRUE);
    ok(GetForegroundWindow() != hwnd, "The window took the foreground\n");
    ok_int(NcActivate, 0);
    DestroyWindow(hwnd);
    PumpMessages();
    return 0;
}

static DWORD WINAPI HiddenWindowThread(LPVOID Parameter)
{
    HWND hwnd, hwndNext;

    ConsumeStartupRight();
    hwnd = CreateTestWindow(TRUE);
    GiveForeground(hwnd);
    ok(GetForegroundWindow() == hwnd, "The window is not the foreground window\n");
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    PumpMessages();
    ok(GetActiveWindow() == hwnd, "The hidden window is not active\n");
    GiveForeground(HelperWindow);

    NcActivate = -1;
    hwndNext = CreateTestWindow(TRUE);
    ok(GetForegroundWindow() == hwndNext, "The next window did not take the foreground\n");
    ok_int(NcActivate, 1);
    DestroyWindow(hwndNext);
    DestroyWindow(hwnd);
    PumpMessages();
    return 0;
}

static HWND CreateHiddenPopup(void)
{
    return CreateWindowExW(0, L"ForegroundRetakeClass", L"ForegroundRetake", WS_POPUP,
                           10, 10, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static DWORD WINAPI HiddenSetWindowPosThread(LPVOID Parameter)
{
    HWND hwnd, hwndNext;

    hwnd = CreateHiddenPopup();
    SetWindowPos(hwnd, NULL, 0, 0, 100, 100, 0);
    PumpMessages();
    ok(GetForegroundWindow() == hwnd, "The hidden popup is not the foreground window\n");
    ok(!IsWindowVisible(hwnd), "The popup is visible\n");
    GiveForeground(HelperWindow);

    NcActivate = -1;
    hwndNext = CreateTestWindow(TRUE);
    ok(GetForegroundWindow() == hwndNext, "The next window did not take the foreground\n");
    ok_int(NcActivate, 1);
    DestroyWindow(hwndNext);
    DestroyWindow(hwnd);
    PumpMessages();
    return 0;
}

static DWORD WINAPI HiddenSetFocusThread(LPVOID Parameter)
{
    HWND hwnd, hwndNext;

    hwnd = CreateHiddenPopup();
    SetFocus(hwnd);
    PumpMessages();
    ok(GetForegroundWindow() == hwnd, "The hidden popup is not the foreground window\n");
    DestroyWindow(hwnd);
    WaitForForeground();

    NcActivate = -1;
    hwndNext = CreateTestWindow(TRUE);
    ok(GetForegroundWindow() != hwndNext, "The next window took the foreground\n");
    ok_int(NcActivate, 0);
    DestroyWindow(hwndNext);
    PumpMessages();
    return 0;
}

static void RunThread(LPTHREAD_START_ROUTINE Routine)
{
    HANDLE thread;

    GiveForeground(HelperWindow);
    thread = CreateThread(NULL, 0, Routine, NULL, 0, NULL);
    ok(thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!thread)
        return;
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
}

START_TEST(ForegroundRetake)
{
    WNDCLASSW wc = { 0 };
    HANDLE ready, helper;
    DWORD tid;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ForegroundRetakeClass";
    RegisterClassW(&wc);

    ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    helper = CreateThread(NULL, 0, HelperThread, ready, 0, &tid);
    WaitForSingleObject(ready, INFINITE);
    if (GetForegroundWindow() != HelperWindow)
    {
        skip("The helper window is not the foreground window\n");
    }
    else
    {
        RunThread(RestoreThread);
        RunThread(HiddenDialogThread);
        RunThread(HiddenWindowThread);
        RunThread(HiddenSetWindowPosThread);
        RunThread(HiddenSetFocusThread);
    }

    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    CloseHandle(ready);
    UnregisterClassW(L"ForegroundRetakeClass", GetModuleHandleW(NULL));
}
