/*
 * LeewayUI Ribbon Skeleton Test Window
 * LGPL-2.1+
 *
 * Minimal test window demonstrating:
 * - One tab, one group, one large button
 * - Command table driven model
 * - Theme integration
 * - DPI awareness
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <leewayui/ribbon.h>
#include <leewayui/theme.h>
#include <leewayui/dpi.h>

/* Application state */
struct AppState {
    HWND main_window;
    ILeewayRibbon *ribbon;
    ILeewayTheme *theme;
    UINT32 current_dpi;
    HWND client_area;
};

static AppState g_app_state = {0};

/* Command table for the test window */
static const LeewayUICommand g_commands[] = {
    {1, L"Open", L"Open an image", L"O", 1, TRUE, FALSE, TRUE},
    {2, L"Save", L"Save the image", L"S", 2, TRUE, FALSE, TRUE},
    {3, L"Print", L"Print the image", L"P", 3, FALSE, FALSE, TRUE},
};

static const UINT32 g_command_count = sizeof(g_commands) / sizeof(g_commands[0]);

/* Command handlers */
static HRESULT OnCommandExecute(UINT32 command_id, void *app_context)
{
    switch (command_id) {
    case 1:
        MessageBox(NULL, L"Open command executed", L"Ribbon Test", MB_OK);
        break;
    case 2:
        MessageBox(NULL, L"Save command executed", L"Ribbon Test", MB_OK);
        break;
    case 3:
        MessageBox(NULL, L"Print command executed", L"Ribbon Test", MB_OK);
        break;
    }
    return S_OK;
}

static HRESULT OnCommandState(UINT32 command_id, LeewayUICommand *out_state, void *app_context)
{
    /* Return the command's state */
    if (command_id < 1 || command_id > g_command_count) {
        return E_INVALIDARG;
    }

    *out_state = g_commands[command_id - 1];
    return S_OK;
}

/* Main window procedure */
static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        AppState *app = (AppState *)lparam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)app);

        app->current_dpi = LeewayDpiGetForWindow(hwnd);

        /* Create theme */
        if (SUCCEEDED(LeewayThemeCreate(&app->theme))) {
            /* Theme created successfully */
        }

        /* Create ribbon control */
        if (SUCCEEDED(LeewayRibbonCreate(
            hwnd,
            g_commands, g_command_count,
            OnCommandExecute,
            OnCommandState,
            app,
            &app->ribbon
        ))) {
            /* Ribbon created successfully */
        }

        /* Create client area (placeholder) */
        app->client_area = CreateWindow(
            L"STATIC", L"Client area (placeholder for image/content)",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 60, 500, 300,
            hwnd, NULL, GetModuleHandle(NULL), NULL
        );

        return 0;
    }
    case WM_SIZE: {
        AppState *app = (AppState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!app) return 0;

        int width = GET_X_LPARAM(lparam);
        int height = GET_Y_LPARAM(lparam);

        if (app->ribbon) {
            UINT32 ribbon_height = LeewayRibbonGetHeight(app->ribbon);
            if (app->client_area) {
                SetWindowPos(app->client_area, NULL,
                    0, ribbon_height, width, height - ribbon_height,
                    SWP_NOZORDER);
            }
        }

        return 0;
    }
    case WM_DPICHANGED: {
        AppState *app = (AppState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!app) return 0;

        app->current_dpi = HIWORD(wparam);
        if (app->ribbon) {
            LeewayRibbonOnDpiChanged(app->ribbon, app->current_dpi);
        }

        /* Let Windows handle the resize */
        RECT *rect = (RECT *)lparam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top,
            rect->right - rect->left, rect->bottom - rect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        return 0;
    }
    case WM_DESTROY: {
        AppState *app = (AppState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!app) return 0;

        if (app->ribbon) {
            LeewayRibbonDestroy(app->ribbon);
        }

        if (app->theme) {
            LeewayThemeDestroy(app->theme);
        }

        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, LPWSTR cmd_line, int show_cmd)
{
    (void)prev_instance;
    (void)cmd_line;

    /* Register main window class */
    WNDCLASS wc = {0};
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LeewayUIRibbonTestWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    /* Create main window */
    g_app_state.main_window = CreateWindowEx(
        WS_EX_APPWINDOW,
        L"LeewayUIRibbonTestWindow",
        L"LeewayUI Ribbon Skeleton Test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
        NULL, NULL, instance, &g_app_state
    );

    if (!g_app_state.main_window) {
        return 1;
    }

    /* Message loop */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
