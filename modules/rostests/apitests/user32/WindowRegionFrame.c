/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the frame updates sent when a window region is removed
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <dwmapi.h>

static HWND Target;
static BOOL Logging;
static HRESULT (WINAPI *pDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static int PosChanging, LocationChange;

static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                               LONG idChild, DWORD idThread, DWORD time)
{
    if (Logging && hwnd == Target && event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW)
        LocationChange++;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (Logging && hwnd == Target && uMsg == WM_WINDOWPOSCHANGING)
        PosChanging++;
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static void TestClear(const char *Name, DWORD Style, BOOL SetFirst, DWORD Policy,
                      int ExpectedPosChanging, int ExpectedLocationChange)
{
    Target = CreateWindowExW(0, L"WindowRegionFrameClass", L"WindowRegionFrame", Style | WS_VISIBLE,
                             100, 100, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(Target != NULL, "%s: CreateWindowExW failed, error %lu\n", Name, GetLastError());
    if (!Target)
        return;
    if (Policy != DWMNCRP_USEWINDOWSTYLE)
    {
        ok(pDwmSetWindowAttribute(Target, DWMWA_NCRENDERING_POLICY, &Policy, sizeof(Policy)) == S_OK,
           "%s: DwmSetWindowAttribute failed\n", Name);
    }
    PumpMessages();
    if (SetFirst)
    {
        SetWindowRgn(Target, CreateRectRgn(0, 0, 150, 150), TRUE);
        PumpMessages();
    }

    PosChanging = LocationChange = 0;
    Logging = TRUE;
    ok(SetWindowRgn(Target, NULL, TRUE) != 0, "%s: SetWindowRgn failed\n", Name);
    Logging = FALSE;
    ok(PosChanging == ExpectedPosChanging, "%s: %d WM_WINDOWPOSCHANGING, expected %d\n",
       Name, PosChanging, ExpectedPosChanging);
    ok(LocationChange == ExpectedLocationChange, "%s: %d location changes, expected %d\n",
       Name, LocationChange, ExpectedLocationChange);

    DestroyWindow(Target);
    PumpMessages();
    Target = NULL;
}

START_TEST(WindowRegionFrame)
{
    WNDCLASSW wc = { 0 };
    HWINEVENTHOOK hook;
    BOOL composition = FALSE;
    HMODULE dwmapi, uxtheme;
    HRESULT (WINAPI *pDwmIsCompositionEnabled)(BOOL *);
    BOOL (WINAPI *pIsThemeActive)(void);

    dwmapi = LoadLibraryW(L"dwmapi.dll");
    uxtheme = LoadLibraryW(L"uxtheme.dll");
    pDwmIsCompositionEnabled = dwmapi ? (void *)GetProcAddress(dwmapi, "DwmIsCompositionEnabled") : NULL;
    pDwmSetWindowAttribute = dwmapi ? (void *)GetProcAddress(dwmapi, "DwmSetWindowAttribute") : NULL;
    pIsThemeActive = uxtheme ? (void *)GetProcAddress(uxtheme, "IsThemeActive") : NULL;
    if (!pDwmIsCompositionEnabled || !pDwmSetWindowAttribute ||
        FAILED(pDwmIsCompositionEnabled(&composition)) || !composition ||
        !pIsThemeActive || !pIsThemeActive())
    {
        skip("Desktop composition or visual styles are not active\n");
        return;
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WindowRegionFrameClass";
    RegisterClassW(&wc);
    hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, GetModuleHandleW(NULL),
                           EventProc, GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_INCONTEXT);
    ok(hook != NULL, "SetWinEventHook failed, error %lu\n", GetLastError());

    TestClear("overlapped", WS_OVERLAPPEDWINDOW, TRUE, DWMNCRP_USEWINDOWSTYLE, 2, 4);
    TestClear("captioned popup", WS_POPUP | WS_CAPTION, TRUE, DWMNCRP_USEWINDOWSTYLE, 2, 4);
    TestClear("overlapped without a region", WS_OVERLAPPEDWINDOW, FALSE, DWMNCRP_USEWINDOWSTYLE, 1, 1);
    TestClear("overlapped with disabled non-client rendering", WS_OVERLAPPEDWINDOW, TRUE, DWMNCRP_DISABLED, 2, 3);
    TestClear("popup", WS_POPUP, TRUE, DWMNCRP_USEWINDOWSTYLE, 1, 1);
    TestClear("bordered popup", WS_POPUP | WS_BORDER, TRUE, DWMNCRP_USEWINDOWSTYLE, 1, 1);

    UnhookWinEvent(hook);
    UnregisterClassW(L"WindowRegionFrameClass", GetModuleHandleW(NULL));
    FreeLibrary(uxtheme);
    FreeLibrary(dwmapi);
}
