/*
 * LeewayUI Ribbon Control — header for v0 skeleton
 * LGPL-2.1+
 */

#pragma once

#include <windows.h>

/* Command table: driven model, single source of truth for ribbon/menu/QAT/accelerators */

typedef struct {
    UINT32 id;              /* stable command id */
    const WCHAR *label;     /* label for UI */
    const WCHAR *tooltip;   /* tooltip text */
    const WCHAR *keytip;    /* 1-3 char keytip */
    UINT32 icon_id;         /* icon identifier (app-specific) */
    BOOL enabled;           /* current enabled state */
    BOOL checked;           /* current checked state (for toggles) */
    BOOL visible;           /* current visible state */
} LeewayUICommand;

/* Handler callback: execute command and query state */
typedef HRESULT (*LeewayUICommandExecuteHandler)(UINT32 command_id, void *app_context);
typedef HRESULT (*LeewayUICommandStateHandler)(UINT32 command_id, LeewayUICommand *out_state, void *app_context);

/* Ribbon control — v0 skeleton: one tab, one group, large buttons */

struct ILeewayRibbon;
typedef struct ILeewayRibbon ILeewayRibbon;

/* Create a minimal ribbon control for a window */
HRESULT LeewayRibbonCreate(
    HWND parent_window,
    const LeewayUICommand *command_table,
    UINT32 command_count,
    LeewayUICommandExecuteHandler execute_handler,
    LeewayUICommandStateHandler state_handler,
    void *app_context,
    ILeewayRibbon **out_ribbon
);

/* Destroy ribbon control */
void LeewayRibbonDestroy(ILeewayRibbon *ribbon);

/* Get the ribbon window handle */
HWND LeewayRibbonGetWindowHandle(ILeewayRibbon *ribbon);

/* Get the ribbon's required height (for layout) */
UINT32 LeewayRibbonGetHeight(ILeewayRibbon *ribbon);

/* Notify ribbon of theme/DPI change */
void LeewayRibbonOnThemeChanged(ILeewayRibbon *ribbon);
void LeewayRibbonOnDpiChanged(ILeewayRibbon *ribbon, UINT32 new_dpi);
