/*
 * PROJECT:    PAINT for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Paint ribbon handlers and integration
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 * PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include "precomp.h"
#include "paint_ribbon.h"

extern CMainWindow mainWindow;
extern BOOL g_showGrid;

/* Execute handler: route ribbon commands to the main window */
static HRESULT WINAPI OnRibbonExecute(UINT32 id, void *app_context)
{
    HWND hWnd = (HWND)app_context;

    /* Framework commands handled by the ribbon itself */
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return S_OK;

    /* Route other commands to the main window as WM_COMMAND */
    SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    return S_OK;
}

/* Query-state handler: read current state from Paint's model */
static HRESULT WINAPI OnRibbonQueryState(UINT32 id, LeewayUICommandState *state, void *app_context)
{
    HWND hWnd = (HWND)app_context;
    if (!hWnd || !state)
        return E_INVALIDARG;

    /* Default: keep table values */
    state->enabled = TRUE;
    state->checked = FALSE;
    state->visible = TRUE;
    state->label = NULL;
    state->tooltip = NULL;

    /* Query specific states based on Paint's model */
    switch (id)
    {
    /* Edit commands: enabled when document exists */
    case IDM_EDITUNDO:
        state->enabled = mainWindow.CanUndo();
        break;

    case IDM_EDITREDO:
        state->enabled = mainWindow.CanRedo();
        break;

    case IDM_EDITCUT:
    case IDM_EDITCOPY:
    case IDM_EDITDELETESELECTION:
    case IDM_EDITINVERTSELECTION:
        /* TODO: check HasSelection from selection model */
        state->enabled = TRUE;
        break;

    case IDM_EDITPASTE:
    case IDM_EDITPASTEFROM:
        state->enabled = mainWindow.CanPaste();
        break;

    /* Clipboard Paste split primary/secondary */
    case IDM_EDITCUT:
    case IDM_EDITCOPY:
        /* Only update enabled; checked is FALSE from table */
        break;

    /* Image commands */
    case IDM_IMAGECROP:
        /* TODO: check HasSelection */
        state->enabled = TRUE;
        break;

    case IDM_IMAGEROTATEMIRROR:
    case 310: /* Rotate dropdown */
        /* TODO: check image exists */
        state->enabled = TRUE;
        break;

    case IDM_IMAGESTRETCHSKEW:
        state->enabled = TRUE;
        break;

    case IDM_IMAGEDELETEIMAGE:
    case IDM_IMAGEINVERTCOLORS:
        /* TODO: check image exists */
        state->enabled = TRUE;
        break;

    case IDM_IMAGEDRAWOPAQUE:
        /* TODO: read from selection model */
        state->checked = FALSE;
        break;

    /* View commands */
    case IDM_VIEWSTATUSBAR:
        /* TODO: check status bar visibility */
        state->checked = TRUE;
        break;

    case IDM_VIEWSHOWGRID:
        state->checked = g_showGrid;
        break;

    case IDM_VIEWSHOWMINIATURE:
        /* TODO: check miniature window visibility */
        state->checked = FALSE;
        break;

    case IDM_VIEWFULLSCREEN:
        /* TODO: check fullscreen state */
        state->checked = FALSE;
        break;

    case IDM_VIEWZOOM100:
        /* TODO: check zoom == 100% */
        state->enabled = TRUE;
        break;

    case IDM_CTRL_PLUS:
    case IDM_CTRL_MINUS:
        /* TODO: check CanZoomIn/Out */
        state->enabled = TRUE;
        break;

    /* Zoom gallery */
    case 380:
        state->enabled = TRUE;
        break;

    /* Tools: radio set */
    case ID_FREESEL:
    case ID_RECTSEL:
    case ID_RUBBER:
    case ID_FILL:
    case ID_COLOR:
    case ID_ZOOM:
    case ID_PEN:
    case ID_BRUSH:
    case ID_AIRBRUSH:
    case ID_TEXT:
    case ID_LINE:
    case ID_BEZIER:
    case ID_RECT:
    case ID_SHAPE:
    case ID_ELLIPSE:
    case ID_RRECT:
        /* TODO: check CurrentTool == id */
        state->checked = FALSE;
        break;

    /* Text formatting (on contextual tab) */
    case IDM_BOLD:
    case IDM_ITALIC:
    case IDM_UNDERLINE:
    case IDM_VERTICAL:
        /* TODO: read from text model when IsTextTool */
        state->checked = FALSE;
        break;

    case 392: /* Opaque background */
    case 393: /* Transparent background */
        /* TODO: read from text model */
        state->checked = FALSE;
        break;

    /* Default: enabled from table */
    default:
        break;
    }

    return S_OK;
}

/* Initialize Paint ribbon: called from CMainWindow::OnCreate */
HRESULT PaintRibbonInitialize(HWND hwndMain, ILeewayRibbon **pRibbon)
{
    if (!hwndMain || !pRibbon)
        return E_INVALIDARG;

    /* Create ribbon with Paint's command table */
    HRESULT hr = LeewayRibbonCreate(hwndMain,
                                    g_paintCommands, _countof(g_paintCommands),
                                    OnRibbonExecute,
                                    OnRibbonQueryState,
                                    (void *)hwndMain,
                                    pRibbon);
    if (FAILED(hr))
        return hr;

    /* Set the layout */
    hr = LeewayRibbonSetLayout(*pRibbon, (LeewayUILayout *)&g_paintLayout);
    if (FAILED(hr))
    {
        LeewayRibbonDestroy(*pRibbon);
        *pRibbon = NULL;
        return hr;
    }

    return S_OK;
}

/* Cleanup Paint ribbon */
void PaintRibbonCleanup(ILeewayRibbon *ribbon)
{
    if (ribbon)
        LeewayRibbonDestroy(ribbon);
}

/* Invalidate command state (called when Paint's model changes) */
void PaintRibbonInvalidateCommand(ILeewayRibbon *ribbon, UINT32 command_id)
{
    if (ribbon)
        LeewayRibbonInvalidateCommand(ribbon, command_id);
}

/* Invalidate all command states */
void PaintRibbonInvalidateAll(ILeewayRibbon *ribbon)
{
    if (ribbon)
        LeewayRibbonInvalidateAll(ribbon);
}
