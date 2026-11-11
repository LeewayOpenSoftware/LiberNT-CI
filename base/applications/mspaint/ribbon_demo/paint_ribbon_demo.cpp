// PROJECT:    Paint Ribbon Standalone Demo for LiberNT
// LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
// PURPOSE:    Standalone demo: window with LeewayUI ribbon, Paint layout
// COPYRIGHT:  Copyright 2026 Leeway Open Software
// PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
//
// No precomp.h, no ATL, no ReactOS dependencies.
// Build: C:\msys64\clang64\bin\clang++.exe -municode -mwindows -DUNICODE -D_UNICODE \
//        -Isdk/include -Ibase/applications/mspaint \
//        sdk/lib/leewayui/ribbon.cpp sdk/lib/leewayui/theme.cpp sdk/lib/leewayui/dpi.cpp \
//        base/applications/mspaint/ribbon_demo/paint_ribbon_demo.cpp \
//        -lgdi32 -luser32 -lcomctl32 -luxtheme -ldwmapi -lshcore -ladvapi32 -lmsimg32 -static \
//        -o paint_ribbon_demo.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include <stdio.h>

#include "leewayui/ribbon.h"
#include "../paint_ribbon.h"

/* Global state */
static ILeewayRibbon *g_pRibbon = NULL;
static HWND g_hWnd = NULL;

/* Execute handler: route ribbon commands */
static HRESULT WINAPI OnExecute(UINT32 id, void *app_context)
{
    HWND hWnd = (HWND)app_context;

    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return S_OK;

    wchar_t msg[256];
    swprintf_s(msg, _countof(msg), L"Command %u (0x%X)", id, id);
    MessageBoxW(hWnd, msg, L"Paint Ribbon Demo", MB_OK | MB_ICONINFORMATION);
    return S_OK;
}

/* Query-state handler: minimal implementation */
static HRESULT WINAPI OnQueryState(UINT32 id, LeewayUICommandState *state, void *app_context)
{
    if (!state) return E_INVALIDARG;

    state->enabled = TRUE;
    state->checked = FALSE;
    state->visible = TRUE;
    state->label = NULL;
    state->tooltip = NULL;

    /* Undo/Redo always disabled for demo */
    if (id == 220 || id == 221)
        state->enabled = FALSE;

    return S_OK;
}

/* Window message handler */
static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        /* Create ribbon with Paint's layout */
        HRESULT hr = LeewayRibbonCreate(hWnd,
                                        g_paintCommands, _countof(g_paintCommands),
                                        OnExecute, OnQueryState,
                                        (void *)hWnd,
                                        &g_pRibbon);
        if (FAILED(hr))
        {
            MessageBoxW(hWnd, L"LeewayRibbonCreate failed", L"Error", MB_OK | MB_ICONERROR);
            return -1;
        }

        /* Set Paint's layout */
        hr = LeewayRibbonSetLayout(g_pRibbon, (LeewayUILayout *)&g_paintLayout);
        if (FAILED(hr))
        {
            MessageBoxW(hWnd, L"LeewayRibbonSetLayout failed", L"Error", MB_OK | MB_ICONERROR);
            LeewayRibbonDestroy(g_pRibbon);
            g_pRibbon = NULL;
            return -1;
        }

        /* Set zoom gallery collection */
        static const LeewayUICollectionItem zoomItems[] = {
            { 234, L"12.5 %", 0 },
            { 235, L"25 %", 0 },
            { 236, L"50 %", 0 },
            { 237, L"100 %", 0 },
            { 238, L"200 %", 0 },
            { 239, L"400 %", 0 },
            { 240, L"800 %", 0 },
        };
        LeewayRibbonSetCollection(g_pRibbon, 380, zoomItems, _countof(zoomItems), 0);

        return 0;
    }

    case WM_DESTROY:
        if (g_pRibbon)
        {
            LeewayRibbonDestroy(g_pRibbon);
            g_pRibbon = NULL;
        }
        PostQuitMessage(0);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);
        UINT ribbonHeight = g_pRibbon ? LeewayRibbonGetHeight(g_pRibbon) : 0;

        /* Draw separator line below ribbon */
        RECT contentRect = rc;
        contentRect.top = ribbonHeight;

        if (contentRect.top < contentRect.bottom)
        {
            FillRect(hdc, &contentRect, (HBRUSH)(COLOR_WINDOW + 1));

            HPEN hpen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
            HPEN hpenOld = (HPEN)SelectObject(hdc, hpen);
            MoveToEx(hdc, 0, ribbonHeight, NULL);
            LineTo(hdc, rc.right, ribbonHeight);
            SelectObject(hdc, hpenOld);
            DeleteObject(hpen);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (g_pRibbon)
            InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

/* Entry point */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR pCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)pCmdLine;

    /* Register window class */
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PaintRibbonDemo";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_VREDRAW | CS_HREDRAW;

    if (!RegisterClassW(&wc))
    {
        MessageBoxW(NULL, L"RegisterClassW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create window */
    g_hWnd = CreateWindowW(L"PaintRibbonDemo", L"Paint Ribbon Demo",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           900, 600,
                           NULL, NULL, hInstance, NULL);

    if (!g_hWnd)
    {
        MessageBoxW(NULL, L"CreateWindowW failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Ensure window is shown (at least SW_SHOW) */
    int show = nCmdShow;
    if (show == 0) show = SW_SHOW;

    ShowWindow(g_hWnd, show);
    UpdateWindow(g_hWnd);
    SetForegroundWindow(g_hWnd);

    /* Message loop */
    MSG msg = {};
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, NULL, 0, 0)) != 0)
    {
        if (bRet == -1)
            break;

        if (g_pRibbon && LeewayRibbonPreTranslateMessage(g_pRibbon, &msg))
            continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
