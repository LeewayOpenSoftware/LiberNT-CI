/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test window demonstrating ribbon, command table, theme, and DPI awareness
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
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
    UINT32 current_dpi;
    HWND client_area;
    BOOL toggle_state;
};

static AppState g_app_state = {0};

/* Command table: showcasing all F1 control types */
static LeewayUICommand g_commands[] = {
    {1, L"Open", L"Open file", L"O", 0, TRUE, FALSE, TRUE},
    {2, L"Save", L"Save file", L"S", 0, TRUE, FALSE, TRUE},
    {3, L"Toggle", L"Toggle state", L"T", 0, TRUE, FALSE, TRUE},
    {4, L"Split", L"Split action", L"X", 0, TRUE, FALSE, TRUE},
    {5, L"Dropdown", L"Pick item", L"D", 0, TRUE, FALSE, TRUE},
    {6, L"Print", L"Print output", L"P", 0, FALSE, FALSE, TRUE},
};

static const UINT32 g_command_count = sizeof(g_commands) / sizeof(g_commands[0]);

/* Build test layout with multiple control types and QAT */
static void BuildTestLayout(ILeewayRibbon *ribbon)
{
    /* Placements for first group */
    static LeewayUIPlacement placements_group1[] = {
        {1, LWUI_CONTROL_BUTTON_LARGE, L"O", NULL},
        {2, LWUI_CONTROL_BUTTON_LARGE, L"S", NULL},
        {3, LWUI_CONTROL_BUTTON_TOGGLE, L"T", NULL},
    };
    placements_group1[0].sibling = &placements_group1[1];
    placements_group1[1].sibling = &placements_group1[2];

    /* Placements for second group */
    static LeewayUIPlacement placements_group2[] = {
        {4, LWUI_CONTROL_BUTTON_SPLIT, L"X", NULL},
        {5, LWUI_CONTROL_BUTTON_DROPDOWN, L"D", NULL},
        {6, LWUI_CONTROL_BUTTON_SMALL, L"Prnt", NULL},
    };
    placements_group2[0].sibling = &placements_group2[1];
    placements_group2[1].sibling = &placements_group2[2];

    /* Groups */
    static LeewayUIGroup groups[] = {
        {L"File", 1, 3, &placements_group1[0], NULL},
        {L"Tools", 1, 3, &placements_group2[0], NULL},
    };
    groups[0].sibling = &groups[1];

    /* Tabs */
    static LeewayUITab tabs[] = {
        {1, L"Home", L"H", &groups[0], NULL},
    };

    LeewayRibbonSetLayout(ribbon, &tabs[0]);

    /* Set up QAT */
    static LeewayUIQATItem qat_items[] = {
        {1, LWUI_CONTROL_BUTTON_SMALL},
        {2, LWUI_CONTROL_BUTTON_SMALL},
    };

    static LeewayUIQAT qat = {&qat_items[0], 2};
    LeewayRibbonSetQAT(ribbon, &qat);

    /* Set up app menu */
    static LeewayUIAppMenuItem app_menu_items[] = {
        {1, LWUI_CONTROL_BUTTON_SMALL, L"Open", NULL},
        {2, LWUI_CONTROL_BUTTON_SMALL, L"Save", NULL},
        {6, LWUI_CONTROL_BUTTON_SMALL, L"Print", NULL},
    };
    app_menu_items[0].sibling = &app_menu_items[1];
    app_menu_items[1].sibling = &app_menu_items[2];

    static LeewayUIAppMenu app_menu = {&app_menu_items[0]};
    LeewayRibbonSetAppMenu(ribbon, &app_menu);
}

/* Command handlers */
static HRESULT OnCommandExecute(UINT32 command_id, void *app_context)
{
    AppState *app = (AppState *)app_context;

    switch (command_id) {
    case 1:
        MessageBox(NULL, L"Open: showing BUTTON_LARGE control", L"F1 MVP", MB_OK);
        break;
    case 2:
        MessageBox(NULL, L"Save: showing BUTTON_LARGE control", L"F1 MVP", MB_OK);
        break;
    case 3:
        /* Toggle state */
        app->toggle_state = !app->toggle_state;
        g_commands[2].checked = app->toggle_state;
        if (app->ribbon) LeewayRibbonInvalidateRegion(app->ribbon, NULL);
        break;
    case 4:
        MessageBox(NULL, L"Split: showing BUTTON_SPLIT control", L"F1 MVP", MB_OK);
        break;
    case 5:
        MessageBox(NULL, L"Dropdown: showing BUTTON_DROPDOWN control", L"F1 MVP", MB_OK);
        break;
    case 6:
        MessageBox(NULL, L"Print: showing BUTTON_SMALL control", L"F1 MVP", MB_OK);
        break;
    }
    return S_OK;
}

static HRESULT OnCommandState(UINT32 command_id, LeewayUICommand *out_state, void *app_context)
{
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

        app->current_dpi = 96;  /* Default, will be updated on WM_DPICHANGED */
        app->toggle_state = FALSE;

        /* Create ribbon control */
        if (SUCCEEDED(LeewayRibbonCreate(
            hwnd,
            g_commands, g_command_count,
            OnCommandExecute,
            OnCommandState,
            app,
            &app->ribbon
        ))) {
            /* Set theme */
            LeewayUITheme theme;
            theme.text_color = RGB(0, 0, 0);
            theme.bg_color = RGB(240, 240, 240);
            theme.border_color = RGB(180, 180, 180);
            theme.hover_color = RGB(220, 220, 255);
            theme.pressed_color = RGB(100, 100, 200);
            theme.disabled_color = RGB(150, 150, 150);
            theme.dark_mode = FALSE;

            LeewayRibbonSetTheme(app->ribbon, &theme);
            LeewayRibbonSetDPI(app->ribbon, app->current_dpi);

            /* Build and set layout */
            BuildTestLayout(app->ribbon);
        }

        /* Create client area (placeholder) */
        app->client_area = CreateWindow(
            L"STATIC", L"LeewayUI F1 MVP: shows large/small/toggle/split/dropdown + QAT",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 120, 500, 300,
            hwnd, NULL, GetModuleHandle(NULL), NULL
        );

        return 0;
    }
    case WM_SIZE: {
        AppState *app = (AppState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!app) return 0;

        int width = (int)(short)LOWORD(lparam);
        int height = (int)(short)HIWORD(lparam);

        if (app->client_area) {
            /* Ribbon is ~120px tall (QAT ~20px + tabs ~24px + groups ~80px) */
            SetWindowPos(app->client_area, NULL,
                0, 120, width, height - 120,
                SWP_NOZORDER);
        }

        return 0;
    }
    case WM_DPICHANGED: {
        AppState *app = (AppState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!app) return 0;

        app->current_dpi = HIWORD(wparam);
        if (app->ribbon) {
            LeewayRibbonSetDPI(app->ribbon, app->current_dpi);
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
        L"LeewayUI F1 Framework MVP - Tabs, Groups, Controls, QAT",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
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
