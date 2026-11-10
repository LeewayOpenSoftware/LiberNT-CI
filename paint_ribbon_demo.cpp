/*
 * PROJECT:    Paint Ribbon Demo for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Standalone demo: window with LeewayUI ribbon, Paint layout, no Paint internals
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 * PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Build: clang++ -municode -mwindows -DUNICODE -D_UNICODE -Isdk/include \
 *          -Ibase/applications/mspaint sdk/lib/leewayui/*.cpp paint_ribbon_demo.cpp \
 *          -lgdi32 -luser32 -lcomctl32 -luxtheme -ldwmapi -lshcore -ladvapi32 \
 *          -lmsimg32 -static
 */

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include "sdk/include/leewayui/ribbon.h"
#include "base/applications/mspaint/paint_ribbon.h"

/* Forward declarations */
class CDemoWindow;
static CDemoWindow *g_pDemoWindow = NULL;

/* Demo command table (subset of Paint) */
static const LeewayUICommand g_demoCommands[] = {
    { 201, L"New", L"New image", L"N", 0, TRUE, FALSE, TRUE },
    { 202, L"Open", L"Open image", L"O", 0, TRUE, FALSE, TRUE },
    { 203, L"Save", L"Save image", L"S", 0, TRUE, FALSE, TRUE },
    { 204, L"Save as", L"Save image as", L"A", 0, TRUE, FALSE, TRUE },
    { 220, L"Undo", L"Undo action", L"U", 0, TRUE, FALSE, TRUE },
    { 221, L"Redo", L"Redo action", L"Y", 0, TRUE, FALSE, TRUE },
    { 222, L"Cut", L"Cut to clipboard", L"X", 0, TRUE, FALSE, TRUE },
    { 223, L"Copy", L"Copy to clipboard", L"C", 0, TRUE, FALSE, TRUE },
    { 224, L"Paste", L"Paste from clipboard", L"V", 0, TRUE, FALSE, TRUE },
    { 280, L"Bold", L"Bold", L"B", 0, TRUE, FALSE, TRUE },
    { 281, L"Italic", L"Italic", L"I", 0, TRUE, FALSE, TRUE },
    { 282, L"Underline", L"Underline", L"U", 0, TRUE, FALSE, TRUE },
    { 283, L"Vertical", L"Vertical text", L"VT", 0, TRUE, FALSE, TRUE },
    { 290, L"Zoom in", L"Zoom in", L"I", 0, TRUE, FALSE, TRUE },
    { 291, L"Zoom out", L"Zoom out", L"O", 0, TRUE, FALSE, TRUE },
    { 237, L"100 %", L"Zoom to 100 percent", L"1", 0, TRUE, FALSE, TRUE },
    { 241, L"Full screen", L"Toggle full screen", L"F", 0, TRUE, FALSE, TRUE },
    { 242, L"Gridlines", L"Show gridlines", L"G", 0, TRUE, FALSE, TRUE },
    { 232, L"Status bar", L"Show status bar", L"S", 0, TRUE, FALSE, TRUE },
    { 243, L"Thumbnail", L"Show thumbnail", L"T", 0, TRUE, FALSE, TRUE },
    { 310, L"Rotate", L"Rotate image", L"RO", 0, TRUE, FALSE, TRUE },
    { 311, L"Rotate right 90°", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 312, L"Rotate left 90°", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 313, L"Rotate 180°", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 314, L"Flip vertical", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 315, L"Flip horizontal", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 320, L"Brushes", L"Select brush", L"B", 0, TRUE, FALSE, TRUE },
    { 321, L"Brush", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 322, L"Airbrush", NULL, NULL, 0, TRUE, FALSE, FALSE },
    { 340, L"Shapes", L"Select shape", L"SH", 0, TRUE, FALSE, TRUE },
    { 360, L"Size", L"Line width", L"SZ", 0, TRUE, FALSE, TRUE },
    { 370, L"Color 1", L"Foreground color", L"C1", 0, TRUE, FALSE, TRUE },
    { 371, L"Color 2", L"Background color", L"C2", 0, TRUE, FALSE, TRUE },
    { 372, L"Palette", L"Palette", L"PA", 0, TRUE, FALSE, TRUE },
    { 380, L"Zoom level", L"Select zoom level", L"Z", 0, TRUE, FALSE, TRUE },
    { 381, L"Rulers", L"Show rulers", L"R", 0, TRUE, FALSE, TRUE },
    { 390, L"Font family", L"Select font", L"FF", 0, TRUE, FALSE, TRUE },
    { 391, L"Font size", L"Select font size", L"FS", 0, TRUE, FALSE, TRUE },
    { 392, L"Opaque", L"Opaque text background", L"O", 0, TRUE, FALSE, TRUE },
    { 393, L"Transparent", L"Transparent text background", L"R", 0, TRUE, FALSE, TRUE },
};

/* Execute handler */
static HRESULT WINAPI OnDemoExecute(UINT32 id, void *app_context)
{
    HWND hWnd = (HWND)app_context;
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return S_OK;

    wchar_t msg[256];
    swprintf_s(msg, _countof(msg), L"Command %u executed", id);
    MessageBoxW(hWnd, msg, L"Paint Ribbon Demo", MB_OK | MB_ICONINFORMATION);
    return S_OK;
}

/* Query-state handler */
static HRESULT WINAPI OnDemoQueryState(UINT32 id, LeewayUICommandState *state, void *app_context)
{
    if (!state) return E_INVALIDARG;

    state->enabled = TRUE;
    state->checked = FALSE;
    state->visible = TRUE;
    state->label = NULL;
    state->tooltip = NULL;

    switch (id)
    {
    case 220: /* Undo */
    case 221: /* Redo */
        state->enabled = FALSE;
        break;

    case 224: /* Paste */
        state->enabled = IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB);
        break;

    case 242: /* Gridlines */
        state->checked = FALSE;
        break;

    default:
        break;
    }

    return S_OK;
}

/* Demo window class */
class CDemoWindow
{
public:
    static ATOM Register();
    HWND Create(HINSTANCE hInst);
    void Destroy();
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    ILeewayRibbon *m_pRibbon;

    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

ATOM CDemoWindow::Register()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PaintRibbonDemo";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_VREDRAW | CS_HREDRAW;
    return RegisterClassW(&wc);
}

HWND CDemoWindow::Create(HINSTANCE hInst)
{
    m_pRibbon = NULL;

    HWND hWnd = CreateWindowW(L"PaintRibbonDemo", L"Paint Ribbon Demo",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              800, 600, NULL, NULL, hInst, this);
    return hWnd;
}

void CDemoWindow::Destroy()
{
    if (m_pRibbon)
    {
        LeewayRibbonDestroy(m_pRibbon);
        m_pRibbon = NULL;
    }
}

LRESULT CDemoWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HWND hWnd = GetParent(NULL);
        for (HWND h = GetActiveWindow(); h; h = GetParent(h))
            hWnd = h;

        HRESULT hr = LeewayRibbonCreate(
            (HWND)lParam,
            g_demoCommands, _countof(g_demoCommands),
            OnDemoExecute, OnDemoQueryState,
            (void *)lParam,
            &m_pRibbon);

        if (FAILED(hr))
            return -1;

        LeewayRibbonSetLayout(m_pRibbon, (LeewayUILayout *)&g_paintLayout);

        /* Set demo collections */
        static const LeewayUICollectionItem zoomItems[] = {
            { 234, L"12.5 %", 0 },
            { 235, L"25 %", 0 },
            { 236, L"50 %", 0 },
            { 237, L"100 %", 0 },
            { 238, L"200 %", 0 },
            { 239, L"400 %", 0 },
            { 240, L"800 %", 0 },
        };
        LeewayRibbonSetCollection(m_pRibbon, 380, zoomItems, _countof(zoomItems), 0);

        return 0;
    }

    case WM_DESTROY:
        Destroy();
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
    {
        if (m_pRibbon)
        {
            RECT rc;
            GetClientRect((HWND)lParam, &rc);
            UINT ribbonHeight = LeewayRibbonGetHeight(m_pRibbon);
            /* Demo: just paint a rect for the ribbon area */
        }
        return 0;
    }

    default:
        return DefWindowProcW((HWND)lParam, msg, wParam, lParam);
    }
}

LRESULT CALLBACK CDemoWindow::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CDemoWindow *pThis = NULL;

    if (msg == WM_CREATE)
    {
        CREATESTRUCTW *pCreate = (CREATESTRUCTW *)lParam;
        pThis = (CDemoWindow *)pCreate->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    }
    else
    {
        pThis = (CDemoWindow *)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    }

    if (pThis)
        return pThis->HandleMessage(msg, wParam, lParam);

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* Entry point */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR pCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)pCmdLine;

    CDemoWindow::Register();

    CDemoWindow demoWindow;
    HWND hWnd = demoWindow.Create(hInstance);
    if (!hWnd)
        return 1;

    g_pDemoWindow = &demoWindow;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
