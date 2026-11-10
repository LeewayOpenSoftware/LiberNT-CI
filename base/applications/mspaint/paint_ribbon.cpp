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

    /* Initialize collections */

    /* Rotate dropdown (310): rotate right 90, rotate left 90, rotate 180, flip vertical, flip horizontal */
    static const LeewayUICollectionItem rotateItems[] = {
        { 311, L"Rotate right 90°", 0 },
        { 312, L"Rotate left 90°", 0 },
        { 313, L"Rotate 180°", 0 },
        { 314, L"Flip vertical", 0 },
        { 315, L"Flip horizontal", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 310, rotateItems, _countof(rotateItems), 0);

    /* Save as format dropdown (204): PNG, JPEG, BMP, GIF, Other */
    static const LeewayUICollectionItem saveAsItems[] = {
        { 301, L"PNG", 0 },
        { 302, L"JPEG", 0 },
        { 303, L"BMP", 0 },
        { 304, L"GIF", 0 },
        { 305, L"Other formats", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, IDM_FILESAVEAS, saveAsItems, _countof(saveAsItems), 0);

    /* Zoom gallery (380): 12.5%, 25%, 50%, 100%, 200%, 400%, 800% */
    static const LeewayUICollectionItem zoomItems[] = {
        { 234, L"12.5 %", 0 },
        { 235, L"25 %", 0 },
        { 236, L"50 %", 0 },
        { 237, L"100 %", 0 },
        { 238, L"200 %", 0 },
        { 239, L"400 %", 0 },
        { 240, L"800 %", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 380, zoomItems, _countof(zoomItems), 0);

    /* Brush gallery (320): brush types (321-322 for brush/airbrush, plus style combinations) */
    static const LeewayUICollectionItem brushItems[] = {
        { 321, L"Brush", 0 },
        { 322, L"Airbrush", 0 },
        /* TODO: enumerate brush styles from tools model for 323-339 */
    };
    LeewayRibbonSetCollection(*pRibbon, 320, brushItems, _countof(brushItems), 0);

    /* Shapes gallery (340): line/curve/rect/polygon/ellipse/rounded-rect */
    static const LeewayUICollectionItem shapeItems[] = {
        { 610, L"Line", 0 },
        { 611, L"Curve", 0 },
        { 612, L"Rectangle", 0 },
        { 613, L"Polygon", 0 },
        { 614, L"Ellipse", 0 },
        { 615, L"Rounded rectangle", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 340, shapeItems, _countof(shapeItems), 0);

    /* Outline/Fill options (345-346): shape outline/fill styles */
    static const LeewayUICollectionItem outlineItems[] = {
        { 347, L"Outline only", 0 },
        { 348, L"Outline and fill", 0 },
        { 349, L"Fill only", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 345, outlineItems, _countof(outlineItems), 0);

    static const LeewayUICollectionItem fillItems[] = {
        { 350, L"Outline only", 0 },
        { 351, L"Outline and fill", 0 },
        { 352, L"Fill only", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 346, fillItems, _countof(fillItems), 0);

    /* Size dropdown (360): line widths 1-5 px (361-365) */
    static const LeewayUICollectionItem sizeItems[] = {
        { 361, L"1 px", 0 },
        { 362, L"2 px", 0 },
        { 363, L"3 px", 0 },
        { 364, L"4 px", 0 },
        { 365, L"5 px", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 360, sizeItems, _countof(sizeItems), 0);

    /* Palette (372): basic palette colors (16 standard colors for demo) */
    static const LeewayUICollectionItem paletteItems[] = {
        { 1001, L"Black", 0 },
        { 1002, L"Dark red", 0 },
        { 1003, L"Dark green", 0 },
        { 1004, L"Dark yellow", 0 },
        { 1005, L"Dark blue", 0 },
        { 1006, L"Dark magenta", 0 },
        { 1007, L"Dark cyan", 0 },
        { 1008, L"Light gray", 0 },
        { 1009, L"Dark gray", 0 },
        { 1010, L"Red", 0 },
        { 1011, L"Green", 0 },
        { 1012, L"Yellow", 0 },
        { 1013, L"Blue", 0 },
        { 1014, L"Magenta", 0 },
        { 1015, L"Cyan", 0 },
        { 1016, L"White", 0 },
    };
    LeewayRibbonSetCollection(*pRibbon, 372, paletteItems, _countof(paletteItems), 0);

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

/* Activate/deactivate Text contextual tab (called when tool changes) */
void PaintRibbonSetTextToolActive(ILeewayRibbon *ribbon, BOOL active)
{
    if (ribbon)
        LeewayRibbonSetContext(ribbon, L"IsTextTool", active);
}
