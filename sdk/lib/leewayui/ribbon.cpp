/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Ribbon control F1 implementation with tabs, groups, buttons, galleries, QAT, app menu
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#define COBJMACROS
#include <windows.h>
#include <leewayui/ribbon.h>
#include <stdlib.h>
#include <string.h>

/* Rendering constants */
#define RIBBON_TAB_HEIGHT 24
#define RIBBON_GROUP_HEIGHT 80
#define BUTTON_LARGE_SIZE 32
#define BUTTON_SMALL_SIZE 16
#define BUTTON_PADDING 4
#define GROUP_PADDING 8
#define TAB_LABEL_HEIGHT 16

/* Internal ribbon implementation structure */
typedef struct ILeewayRibbon {
    HWND window_handle;
    const LeewayUICommand *command_table;
    UINT32 command_count;
    LeewayUICommandExecuteHandler execute_handler;
    LeewayUICommandStateHandler state_handler;
    void *app_context;

    /* F1 Layout */
    LeewayUITab *tabs;
    LeewayUIQAT *qat;
    LeewayUIAppMenu *app_menu;
    UINT32 active_tab_index;

    /* Theme and rendering */
    LeewayUITheme theme;
    UINT32 dpi;
    FLOAT scale_factor;

    /* Interaction state */
    UINT32 hovered_command_id;
    UINT32 pressed_command_id;
    BOOL keytip_mode;
} ILeewayRibbon;

/* Helper: get command state */
static HRESULT GetCommandState(ILeewayRibbon *ribbon, UINT32 cmd_id, LeewayUICommand *out)
{
    if (!ribbon || !out) return E_INVALIDARG;
    if (!ribbon->state_handler) return E_NOTIMPL;
    return ribbon->state_handler(cmd_id, out, ribbon->app_context);
}

/* Helper: render a large button (32x32 icon + label) */
static void RenderButtonLarge(HDC hdc, RECT *rc, LeewayUICommand *cmd, BOOL hovered, BOOL pressed, LeewayUITheme *theme)
{
    if (!cmd) return;

    COLORREF bg = pressed ? theme->pressed_color : (hovered ? theme->hover_color : theme->bg_color);
    HBRUSH btn_brush = CreateSolidBrush(bg);
    FillRect(hdc, rc, btn_brush);
    DeleteObject(btn_brush);

    HBRUSH border_brush = CreateSolidBrush(theme->border_color);
    FrameRect(hdc, rc, border_brush);
    DeleteObject(border_brush);

    SetTextColor(hdc, cmd->enabled ? theme->text_color : theme->disabled_color);
    SetBkMode(hdc, TRANSPARENT);
    RECT text_rc = *rc;
    text_rc.top += 24;
    DrawTextW(hdc, cmd->label, -1, &text_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

/* Helper: render a small button (16x16 icon, optional label) */
static void RenderButtonSmall(HDC hdc, RECT *rc, LeewayUICommand *cmd, BOOL hovered, BOOL pressed, LeewayUITheme *theme)
{
    if (!cmd) return;

    COLORREF bg = pressed ? theme->pressed_color : (hovered ? theme->hover_color : theme->bg_color);
    HBRUSH btn_brush = CreateSolidBrush(bg);
    FillRect(hdc, rc, btn_brush);
    DeleteObject(btn_brush);

    HBRUSH border_brush = CreateSolidBrush(theme->border_color);
    FrameRect(hdc, rc, border_brush);
    DeleteObject(border_brush);

    SetTextColor(hdc, cmd->enabled ? theme->text_color : theme->disabled_color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, cmd->label, -1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* Helper: render a toggle button (same as large but with checked indicator) */
static void RenderButtonToggle(HDC hdc, RECT *rc, LeewayUICommand *cmd, BOOL hovered, BOOL pressed, LeewayUITheme *theme)
{
    if (!cmd) return;

    COLORREF bg = cmd->checked ? theme->pressed_color : (pressed ? theme->pressed_color : (hovered ? theme->hover_color : theme->bg_color));
    HBRUSH btn_brush = CreateSolidBrush(bg);
    FillRect(hdc, rc, btn_brush);
    DeleteObject(btn_brush);

    HBRUSH border_brush = CreateSolidBrush(cmd->checked ? theme->pressed_color : theme->border_color);
    FrameRect(hdc, rc, border_brush);
    DeleteObject(border_brush);

    if (cmd->checked) {
        HPEN check_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        SelectObject(hdc, check_pen);
        MoveToEx(hdc, rc->left + 4, rc->top + 16, NULL);
        LineTo(hdc, rc->left + 10, rc->top + 22);
        LineTo(hdc, rc->left + 28, rc->top + 4);
        DeleteObject(check_pen);
    }

    SetTextColor(hdc, cmd->enabled ? theme->text_color : theme->disabled_color);
    SetBkMode(hdc, TRANSPARENT);
    RECT text_rc = *rc;
    text_rc.top += 24;
    DrawTextW(hdc, cmd->label, -1, &text_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

/* Helper: render a split button (primary action + dropdown arrow) */
static void RenderButtonSplit(HDC hdc, RECT *rc, LeewayUICommand *cmd, BOOL hovered, BOOL pressed, LeewayUITheme *theme)
{
    if (!cmd) return;

    /* Main button area */
    RECT main_rc = *rc;
    main_rc.right -= 12;

    COLORREF bg = pressed ? theme->pressed_color : (hovered ? theme->hover_color : theme->bg_color);
    HBRUSH btn_brush = CreateSolidBrush(bg);
    FillRect(hdc, &main_rc, btn_brush);
    DeleteObject(btn_brush);

    HBRUSH border_brush = CreateSolidBrush(theme->border_color);
    FrameRect(hdc, &main_rc, border_brush);

    /* Dropdown arrow area */
    RECT arrow_rc;
    arrow_rc.left = main_rc.right;
    arrow_rc.top = main_rc.top;
    arrow_rc.right = rc->right;
    arrow_rc.bottom = main_rc.bottom;

    FillRect(hdc, &arrow_rc, btn_brush);
    FrameRect(hdc, &arrow_rc, border_brush);
    DeleteObject(border_brush);
    DeleteObject(btn_brush);

    /* Draw small down arrow */
    HPEN arrow_pen = CreatePen(PS_SOLID, 1, theme->text_color);
    SelectObject(hdc, arrow_pen);
    INT cx = arrow_rc.left + (arrow_rc.right - arrow_rc.left) / 2;
    INT cy = arrow_rc.top + (arrow_rc.bottom - arrow_rc.top) / 2;
    MoveToEx(hdc, cx - 2, cy - 1, NULL);
    LineTo(hdc, cx + 3, cy - 1);
    MoveToEx(hdc, cx - 1, cy + 1, NULL);
    LineTo(hdc, cx + 2, cy + 1);
    DeleteObject(arrow_pen);

    SetTextColor(hdc, cmd->enabled ? theme->text_color : theme->disabled_color);
    SetBkMode(hdc, TRANSPARENT);
    RECT text_rc = *rc;
    text_rc.top += 24;
    text_rc.right = main_rc.right;
    DrawTextW(hdc, cmd->label, -1, &text_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

/* Helper: render a dropdown button (label + arrow) */
static void RenderButtonDropdown(HDC hdc, RECT *rc, LeewayUICommand *cmd, BOOL hovered, BOOL pressed, LeewayUITheme *theme)
{
    if (!cmd) return;

    COLORREF bg = pressed ? theme->pressed_color : (hovered ? theme->hover_color : theme->bg_color);
    HBRUSH btn_brush = CreateSolidBrush(bg);
    FillRect(hdc, rc, btn_brush);
    DeleteObject(btn_brush);

    HBRUSH border_brush = CreateSolidBrush(theme->border_color);
    FrameRect(hdc, rc, border_brush);
    DeleteObject(border_brush);

    SetTextColor(hdc, cmd->enabled ? theme->text_color : theme->disabled_color);
    SetBkMode(hdc, TRANSPARENT);

    /* Draw label and small arrow */
    RECT text_rc = *rc;
    text_rc.right -= 8;
    DrawTextW(hdc, cmd->label, -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* Draw arrow */
    HPEN arrow_pen = CreatePen(PS_SOLID, 1, theme->text_color);
    SelectObject(hdc, arrow_pen);
    INT cx = rc->right - 4;
    INT cy = rc->top + (rc->bottom - rc->top) / 2;
    MoveToEx(hdc, cx - 2, cy - 1, NULL);
    LineTo(hdc, cx + 1, cy - 1);
    MoveToEx(hdc, cx - 1, cy + 1, NULL);
    LineTo(hdc, cx, cy + 1);
    DeleteObject(arrow_pen);
}

/* Main paint handler for ribbon */
static void RibbonPaint(ILeewayRibbon *ribbon, HDC hdc, RECT *rc)
{
    if (!ribbon) return;

    /* Draw background */
    HBRUSH bg_brush = CreateSolidBrush(ribbon->theme.bg_color);
    FillRect(hdc, rc, bg_brush);
    DeleteObject(bg_brush);

    INT content_y = 0;

    /* Render QAT (Quick Access Toolbar) if present */
    if (ribbon->qat && ribbon->qat->count > 0) {
        INT qat_x = 4;
        for (UINT32 i = 0; i < ribbon->qat->count && i < 10; i++) {
            LeewayUICommand qat_cmd;
            if (SUCCEEDED(GetCommandState(ribbon, ribbon->qat->items[i].command_id, &qat_cmd))) {
                RECT qat_btn_rc;
                qat_btn_rc.left = qat_x;
                qat_btn_rc.top = 2;
                qat_btn_rc.right = qat_x + BUTTON_SMALL_SIZE;
                qat_btn_rc.bottom = qat_btn_rc.top + BUTTON_SMALL_SIZE;

                BOOL hovered = (ribbon->hovered_command_id == ribbon->qat->items[i].command_id);
                BOOL pressed = (ribbon->pressed_command_id == ribbon->qat->items[i].command_id);
                RenderButtonSmall(hdc, &qat_btn_rc, &qat_cmd, hovered, pressed, &ribbon->theme);

                qat_x += BUTTON_SMALL_SIZE + 2;
            }
        }
        content_y = BUTTON_SMALL_SIZE + 6;
    }

    /* Render app menu button (File) */
    if (ribbon->app_menu) {
        RECT app_menu_rc;
        app_menu_rc.left = rc->right - 40;
        app_menu_rc.top = 2;
        app_menu_rc.right = rc->right - 2;
        app_menu_rc.bottom = content_y + BUTTON_SMALL_SIZE;

        COLORREF app_menu_bg = RGB(180, 0, 0);
        HBRUSH app_menu_brush = CreateSolidBrush(app_menu_bg);
        FillRect(hdc, &app_menu_rc, app_menu_brush);
        DeleteObject(app_menu_brush);

        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"File", -1, &app_menu_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (!ribbon->tabs) {
        /* Placeholder: no layout set yet */
        SetTextColor(hdc, ribbon->theme.text_color);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"[No layout]", -1, rc, DT_LEFT | DT_TOP);
        return;
    }

    /* Render tabs */
    UINT32 tab_index = 0;
    const LeewayUITab *tab = ribbon->tabs;
    INT tab_x = 8;
    INT tab_y = content_y;

    while (tab && tab_index < 10) {
        RECT tab_rc;
        tab_rc.left = tab_x;
        tab_rc.top = tab_y;
        tab_rc.right = tab_x + 80;
        tab_rc.bottom = tab_rc.top + RIBBON_TAB_HEIGHT - 4;

        BOOL tab_active = (tab_index == ribbon->active_tab_index);
        if (tab_active) {
            HBRUSH tab_brush = CreateSolidBrush(ribbon->theme.border_color);
            FillRect(hdc, &tab_rc, tab_brush);
            DeleteObject(tab_brush);
            SetTextColor(hdc, RGB(255, 255, 255));
        } else {
            SetTextColor(hdc, ribbon->theme.text_color);
        }
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, tab->label, -1, &tab_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        tab_x += 88;
        tab = tab->sibling;
        tab_index++;
    }

    /* Render active tab's groups and controls */
    if (ribbon->active_tab_index == 0 && ribbon->tabs) {
        tab = ribbon->tabs;
        INT group_x = 8;
        INT group_y = tab_y + RIBBON_TAB_HEIGHT;

        const LeewayUIGroup *group = tab->groups;
        while (group) {
            /* Group label */
            RECT group_label_rc;
            group_label_rc.left = group_x;
            group_label_rc.top = group_y + BUTTON_LARGE_SIZE + 4;
            group_label_rc.right = group_x + (BUTTON_LARGE_SIZE + BUTTON_PADDING) * 3;
            group_label_rc.bottom = group_label_rc.top + TAB_LABEL_HEIGHT;

            SetTextColor(hdc, ribbon->theme.text_color);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, group->label, -1, &group_label_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);

            /* Group controls */
            const LeewayUIPlacement *placement = group->controls;
            INT control_x = group_x;
            INT control_y = group_y;
            INT controls_per_row = 1;

            while (placement) {
                RECT btn_rc;
                btn_rc.left = control_x;
                btn_rc.top = control_y;
                btn_rc.right = btn_rc.left + BUTTON_LARGE_SIZE;
                btn_rc.bottom = btn_rc.top + BUTTON_LARGE_SIZE;

                LeewayUICommand cmd_state;
                if (SUCCEEDED(GetCommandState(ribbon, placement->command_id, &cmd_state))) {
                    BOOL hovered = (ribbon->hovered_command_id == placement->command_id);
                    BOOL pressed = (ribbon->pressed_command_id == placement->command_id);

                    switch (placement->kind) {
                    case LWUI_CONTROL_BUTTON_LARGE:
                        RenderButtonLarge(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    case LWUI_CONTROL_BUTTON_SMALL:
                        btn_rc.right = btn_rc.left + BUTTON_SMALL_SIZE;
                        btn_rc.bottom = btn_rc.top + BUTTON_SMALL_SIZE;
                        RenderButtonSmall(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    case LWUI_CONTROL_TOGGLE_LARGE:
                    case LWUI_CONTROL_TOGGLE_SMALL:
                        if (placement->kind == LWUI_CONTROL_TOGGLE_SMALL) {
                            btn_rc.right = btn_rc.left + BUTTON_SMALL_SIZE;
                            btn_rc.bottom = btn_rc.top + BUTTON_SMALL_SIZE;
                        }
                        RenderButtonToggle(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    case LWUI_CONTROL_SPLIT_LARGE:
                    case LWUI_CONTROL_SPLIT_SMALL:
                        if (placement->kind == LWUI_CONTROL_SPLIT_SMALL) {
                            btn_rc.right = btn_rc.left + BUTTON_SMALL_SIZE;
                            btn_rc.bottom = btn_rc.top + BUTTON_SMALL_SIZE;
                        }
                        RenderButtonSplit(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    case LWUI_CONTROL_DROPDOWN:
                        RenderButtonDropdown(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    default:
                        RenderButtonLarge(hdc, &btn_rc, &cmd_state, hovered, pressed, &ribbon->theme);
                        break;
                    }
                }

                control_x += BUTTON_LARGE_SIZE + BUTTON_PADDING;
                controls_per_row++;
                if (controls_per_row > group->max_columns) {
                    control_x = group_x;
                    control_y += BUTTON_LARGE_SIZE + BUTTON_PADDING;
                    controls_per_row = 0;
                }

                placement = placement->sibling;
            }

            group_x += (BUTTON_LARGE_SIZE + BUTTON_PADDING) * 4 + GROUP_PADDING;
            group = group->sibling;
        }
    }
}

/* Window procedure for the ribbon control */
static LRESULT CALLBACK RibbonWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ILeewayRibbon *ribbon = NULL;

    if (msg == WM_CREATE) {
        CREATESTRUCT *create_struct = (CREATESTRUCT *)lparam;
        ribbon = (ILeewayRibbon *)create_struct->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)ribbon);
    } else {
        ribbon = (ILeewayRibbon *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);

        RibbonPaint(ribbon, hdc, &rect);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (ribbon) {
            INT x = (INT)(SHORT)LOWORD(lparam);
            INT y = (INT)(SHORT)HIWORD(lparam);
            /* TODO: map (x,y) to command and set pressed state */
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (ribbon && ribbon->pressed_command_id && ribbon->execute_handler) {
            ribbon->execute_handler(ribbon->pressed_command_id, ribbon->app_context);
            ribbon->pressed_command_id = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

/* API Functions */

HRESULT WINAPI LeewayRibbonSetLayout(ILeewayRibbon *ribbon, const LeewayUITab *tabs)
{
    if (!ribbon) return E_INVALIDARG;
    ribbon->tabs = (LeewayUITab *)tabs;
    if (ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetQAT(ILeewayRibbon *ribbon, const LeewayUIQAT *qat)
{
    if (!ribbon) return E_INVALIDARG;
    ribbon->qat = (LeewayUIQAT *)qat;
    if (ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetAppMenu(ILeewayRibbon *ribbon, const LeewayUIAppMenu *app_menu)
{
    if (!ribbon) return E_INVALIDARG;
    ribbon->app_menu = (LeewayUIAppMenu *)app_menu;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetTheme(ILeewayRibbon *ribbon, const LeewayUITheme *theme)
{
    if (!ribbon || !theme) return E_INVALIDARG;
    ribbon->theme = *theme;
    if (ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *ribbon, UINT32 dpi)
{
    if (!ribbon) return E_INVALIDARG;
    ribbon->dpi = dpi;
    ribbon->scale_factor = dpi / 96.0f;
    if (ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonInvalidateRegion(ILeewayRibbon *ribbon, const RECT *rc)
{
    if (ribbon && ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, rc, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonInvalidateCommand(ILeewayRibbon *ribbon, UINT32 command_id)
{
    if (ribbon && ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

HRESULT WINAPI LeewayRibbonInvalidateAll(ILeewayRibbon *ribbon)
{
    if (ribbon && ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, FALSE);
    }
    return S_OK;
}

UINT32 WINAPI LeewayRibbonGetHeight(ILeewayRibbon *ribbon)
{
    /* QAT (20px) + tabs (24px) + groups (80px) = 124px */
    if (!ribbon) return 0;

    /* TODO: account for minimize state and actual content */
    return 124;
}

BOOL WINAPI LeewayRibbonIsMinimized(ILeewayRibbon *ribbon)
{
    /* TODO: track minimize state via LWUI_CMD_MINIMIZE command */
    return FALSE;
}

HRESULT WINAPI LeewayRibbonMinimizeRibbon(ILeewayRibbon *ribbon, BOOL minimize)
{
    if (!ribbon) return E_INVALIDARG;
    /* TODO: store minimize state and invalidate */
    return S_OK;
}

HRESULT WINAPI LeewayRibbonPersistQAT(ILeewayRibbon *ribbon)
{
    if (!ribbon) return E_INVALIDARG;
    /* TODO: write HKCU\Software\Leeway\<AppName>\QAT Items value */
    return S_OK;
}

HRESULT WINAPI LeewayRibbonLoadQATFromRegistry(ILeewayRibbon *ribbon, HKEY root, const WCHAR *app_name)
{
    if (!ribbon || !root || !app_name) return E_INVALIDARG;
    /* TODO: read HKCU\Software\Leeway\<app_name>\QAT Items, drop stale ids */
    return S_OK;
}

HWND WINAPI LeewayRibbonGetHWND(ILeewayRibbon *ribbon)
{
    return ribbon ? ribbon->window_handle : NULL;
}

HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *ribbon)
{
    if (!ribbon) return E_INVALIDARG;
    if (ribbon->window_handle) {
        DestroyWindow(ribbon->window_handle);
    }
    HeapFree(GetProcessHeap(), 0, ribbon);
    return S_OK;
}

HRESULT LeewayRibbonCreate(
    HWND parent_window,
    const LeewayUICommand *command_table,
    UINT32 command_count,
    LeewayUICommandExecuteHandler execute_handler,
    LeewayUICommandStateHandler state_handler,
    void *app_context,
    ILeewayRibbon **out_ribbon
)
{
    if (!parent_window || !command_table || !out_ribbon) {
        return E_INVALIDARG;
    }

    /* Allocate ribbon structure */
    ILeewayRibbon *ribbon = (ILeewayRibbon *)HeapAlloc(GetProcessHeap(), 0, sizeof(ILeewayRibbon));
    if (!ribbon) {
        return E_OUTOFMEMORY;
    }

    /* Initialize to zero */
    ZeroMemory(ribbon, sizeof(ILeewayRibbon));

    /* Register window class if needed */
    static BOOL class_registered = FALSE;
    if (!class_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = RibbonWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"LeewayUIRibbon";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        class_registered = TRUE;
    }

    /* Create ribbon window */
    HWND ribbon_window = CreateWindowExW(
        0, L"LeewayUIRibbon", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 800, 120,
        parent_window, NULL, GetModuleHandle(NULL), ribbon
    );

    if (!ribbon_window) {
        HeapFree(GetProcessHeap(), 0, ribbon);
        return E_FAIL;
    }

    /* Initialize ribbon structure */
    ribbon->window_handle = ribbon_window;
    ribbon->command_table = command_table;
    ribbon->command_count = command_count;
    ribbon->execute_handler = execute_handler;
    ribbon->state_handler = state_handler;
    ribbon->app_context = app_context;

    /* Initialize theme to light mode defaults */
    ribbon->theme.text_color = RGB(0, 0, 0);
    ribbon->theme.bg_color = RGB(240, 240, 240);
    ribbon->theme.border_color = RGB(200, 200, 200);
    ribbon->theme.hover_color = RGB(220, 220, 255);
    ribbon->theme.pressed_color = RGB(100, 100, 200);
    ribbon->theme.disabled_color = RGB(150, 150, 150);
    ribbon->theme.dark_mode = FALSE;

    /* Initialize DPI */
    ribbon->dpi = 96;
    ribbon->scale_factor = 1.0f;
    ribbon->active_tab_index = 0;

    *out_ribbon = ribbon;
    return S_OK;
}

