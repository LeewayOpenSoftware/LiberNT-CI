/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the nesting limit of user-mode callbacks
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

static HWND hwndA, hwndB;
static HHOOK hHook;
static int Depth, MaxDepth;
static HANDLE hReady;

static LRESULT CALLBACK GetMsgHook(int code, WPARAM wParam, LPARAM lParam)
{
    MSG msg;
    LRESULT ret;

    Depth++;
    if (Depth > MaxDepth)
        MaxDepth = Depth;
    PeekMessageW(&msg, hwndA, 0, 0, PM_NOREMOVE);
    ret = CallNextHookEx(hHook, code, wParam, lParam);
    Depth--;
    return ret;
}

static int MeasureHookDepth(void)
{
    MSG msg;

    hHook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgHook, NULL, GetCurrentThreadId());
    PostMessageW(hwndA, WM_USER, 0, 0);
    PostMessageW(hwndA, WM_USER + 1, 0, 0);
    Depth = MaxDepth = 0;
    GetMessageW(&msg, hwndA, 0, 0);
    UnhookWindowsHookEx(hHook);
    while (PeekMessageW(&msg, hwndA, 0, 0, PM_REMOVE))
        ;
    return MaxDepth;
}

static LRESULT CALLBACK WndProcA(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_APP + 2)
        return wParam ? SendMessageW(hwndB, WM_APP + 1, wParam - 1, 0) : MeasureHookDepth();
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK WndProcB(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_APP + 1)
        return SendMessageW(hwndA, WM_APP + 2, wParam, 0);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static DWORD WINAPI ThreadB(LPVOID Parameter)
{
    MSG msg;

    hwndB = CreateWindowExW(0, L"CallbackNestingB", NULL, WS_POPUP, 0, 0, 10, 10,
                            NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetEvent(hReady);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

START_TEST(CallbackNesting)
{
    WNDCLASSW wc = { 0 };
    HANDLE hThread;
    int k, depth;

    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = WndProcA;
    wc.lpszClassName = L"CallbackNestingA";
    RegisterClassW(&wc);
    wc.lpfnWndProc = WndProcB;
    wc.lpszClassName = L"CallbackNestingB";
    RegisterClassW(&wc);

    hwndA = CreateWindowExW(0, L"CallbackNestingA", NULL, WS_POPUP, 0, 0, 10, 10,
                            NULL, NULL, wc.hInstance, NULL);
    ok(hwndA != NULL, "CreateWindowExW failed, error %lu\n", GetLastError());
    hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    hThread = CreateThread(NULL, 0, ThreadB, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!hwndA || !hThread)
        return;
    WaitForSingleObject(hReady, INFINITE);

    for (k = 0; k <= 8; k++)
    {
        depth = (int)SendMessageW(hwndA, WM_APP + 2, k, 0);
        ok(depth == 31 - k, "%d nested sends: hook depth %d, expected %d\n", k, depth, 31 - k);
    }

    PostMessageW(hwndB, WM_QUIT, 0, 0);
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    CloseHandle(hReady);
    DestroyWindow(hwndA);
}
