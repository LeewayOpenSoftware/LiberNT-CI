/*
 * LeewayUI Ribbon Control — v0 skeleton implementation
 * LGPL-2.1+
 */

#define COBJMACROS
#include <windows.h>
#include <leewayui/ribbon.h>

/* Internal ribbon control structure */
struct ILeewayRibbon {
    HWND window_handle;
    const LeewayUICommand *command_table;
    UINT32 command_count;
    LeewayUICommandExecuteHandler execute_handler;
    LeewayUICommandStateHandler state_handler;
    void *app_context;
};

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

        /* Skeleton: just draw a light gray background and placeholder text */
        RECT rect;
        GetClientRect(hwnd, &rect);

        /* Draw background */
        HBRUSH bg_brush = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(hdc, &rect, bg_brush);
        DeleteObject(bg_brush);

        /* Draw placeholder text indicating this is the ribbon */
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"LeewayUI Ribbon (One Tab, One Group, One Button)", -1, &rect, DT_LEFT | DT_VCENTER);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
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
        0, 0, 500, 60,
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

    *out_ribbon = ribbon;
    return S_OK;
}

void LeewayRibbonDestroy(ILeewayRibbon *ribbon)
{
    if (!ribbon) {
        return;
    }

    if (ribbon->window_handle) {
        DestroyWindow(ribbon->window_handle);
    }

    HeapFree(GetProcessHeap(), 0, ribbon);
}

HWND LeewayRibbonGetWindowHandle(ILeewayRibbon *ribbon)
{
    return ribbon ? ribbon->window_handle : NULL;
}

UINT32 LeewayRibbonGetHeight(ILeewayRibbon *ribbon)
{
    /* Skeleton: fixed 60-pixel height */
    return 60;
}

void LeewayRibbonOnThemeChanged(ILeewayRibbon *ribbon)
{
    if (ribbon && ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, TRUE);
    }
}

void LeewayRibbonOnDpiChanged(ILeewayRibbon *ribbon, UINT32 new_dpi)
{
    if (ribbon && ribbon->window_handle) {
        InvalidateRect(ribbon->window_handle, NULL, TRUE);
    }
}
