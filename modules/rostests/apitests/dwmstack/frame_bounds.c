/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DWMWA_EXTENDED_FRAME_BOUNDS of a frame with a transparent band
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <reactos/dwmframe.h>

#define EXTENDED_FRAME_BOUNDS 9

typedef HRESULT (WINAPI *DWM_GET_ATTRIBUTE)(HWND, DWORD, PVOID, DWORD);

static HRESULT
GetFrameBounds(DWM_GET_ATTRIBUTE Get, HWND hwnd, RECT *Rect)
{
    return Get(hwnd, EXTENDED_FRAME_BOUNDS, Rect, sizeof(*Rect));
}

START_TEST(frame_bounds)
{
    DPI_AWARENESS_CONTEXT Old;
    DWM_GET_ATTRIBUTE Get;
    HMODULE Dwmapi;
    RECT Window, Bounds;
    HWND hwnd;
    HRESULT hr;

    Dwmapi = LoadLibraryW(L"dwmapi.dll");
    Get = Dwmapi ? (DWM_GET_ATTRIBUTE)GetProcAddress(Dwmapi, "DwmGetWindowAttribute") : NULL;
    if (Get == NULL)
    {
        skip("DwmGetWindowAttribute is not available\n");
        return;
    }
    Old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    hwnd = CreateWindowExW(0, L"static", L"frame_bounds", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed, %lu\n", GetLastError());
    if (hwnd == NULL)
        goto Done;
    /* Start from a frame that publishes no band. */
    RemovePropW(hwnd, DWM_PROP_FRAME_INSETS);
    RemovePropW(hwnd, DWM_PROP_FRAME_ALPHA);
    GetWindowRect(hwnd, &Window);
    hr = GetFrameBounds(Get, hwnd, &Bounds);
    if (hr == E_HANDLE)
    {
        skip("composition is off\n");
        goto Destroy;
    }
    ok(hr == S_OK, "hr %#lx\n", hr);

    /* No band published: the bounds are the window rect. */
    ok(EqualRect(&Bounds, &Window), "no band: got %s, window %s\n",
       wine_dbgstr_rect(&Bounds), wine_dbgstr_rect(&Window));

    /* Windows 10 model: 7/0/7/7 at 96 DPI is left out of the visible frame.
     * The band does not depend on DWM_PROP_FRAME_ALPHA. */
    SetPropW(hwnd, DWM_PROP_FRAME_INSETS, (HANDLE)(ULONG_PTR)DWM_FRAME_INSETS_PACK(7, 0, 7, 7));
    hr = GetFrameBounds(Get, hwnd, &Bounds);
    ok(hr == S_OK, "hr %#lx\n", hr);
    {
        WINDOWINFO Info;
        LONG X, Y;

        Info.cbSize = sizeof(Info);
        GetWindowInfo(hwnd, &Info);
        X = min(7, (LONG)Info.cxWindowBorders);
        Y = min(7, (LONG)Info.cyWindowBorders);
        ok(Bounds.left == Window.left + X && Bounds.top == Window.top &&
           Bounds.right == Window.right - X && Bounds.bottom == Window.bottom - Y,
           "band 7/0/7/7: got %s, window %s\n", wine_dbgstr_rect(&Bounds), wine_dbgstr_rect(&Window));
    }

    /* A band wider than the frame is clamped to the window border: never into
     * the client, and on top never into the caption. */
    SetPropW(hwnd, DWM_PROP_FRAME_INSETS, (HANDLE)(ULONG_PTR)DWM_FRAME_INSETS_PACK(200, 200, 200, 200));
    hr = GetFrameBounds(Get, hwnd, &Bounds);
    {
        WINDOWINFO Info;

        Info.cbSize = sizeof(Info);
        GetWindowInfo(hwnd, &Info);
        ok(hr == S_OK &&
           Bounds.left == Window.left + (LONG)Info.cxWindowBorders &&
           Bounds.top == Window.top + (LONG)Info.cyWindowBorders &&
           Bounds.right == Window.right - (LONG)Info.cxWindowBorders &&
           Bounds.bottom == Window.bottom - (LONG)Info.cyWindowBorders,
           "clamped: got %s, window %s, borders %u/%u\n", wine_dbgstr_rect(&Bounds),
           wine_dbgstr_rect(&Window), Info.cxWindowBorders, Info.cyWindowBorders);
    }

    /* Maximized: only the part on the work area is visible. */
    SetPropW(hwnd, DWM_PROP_FRAME_INSETS, (HANDLE)(ULONG_PTR)DWM_FRAME_INSETS_PACK(7, 0, 7, 7));
    ShowWindow(hwnd, SW_MAXIMIZE);
    {
        MONITORINFO Info;
        RECT Expected;

        Info.cbSize = sizeof(Info);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &Info);
        GetWindowRect(hwnd, &Window);
        IntersectRect(&Expected, &Window, &Info.rcWork);
        hr = GetFrameBounds(Get, hwnd, &Bounds);
        ok(hr == S_OK && EqualRect(&Bounds, &Expected), "maximized: got %s, expected %s\n",
           wine_dbgstr_rect(&Bounds), wine_dbgstr_rect(&Expected));
    }

    RemovePropW(hwnd, DWM_PROP_FRAME_INSETS);
Destroy:
    DestroyWindow(hwnd);
Done:
    SetThreadDpiAwarenessContext(Old);
    FreeLibrary(Dwmapi);
}
