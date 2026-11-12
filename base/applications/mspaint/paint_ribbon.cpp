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

static const struct { UINT id; int zoom; } g_zoomIds[] = {
    { IDM_VIEWZOOM125, 125 }, { IDM_VIEWZOOM25, 250 }, { IDM_VIEWZOOM50, 500 }, { IDM_VIEWZOOM100, 1000 },
    { IDM_VIEWZOOM200, 2000 }, { IDM_VIEWZOOM400, 4000 }, { IDM_VIEWZOOM800, 8000 },
};
static const int g_sizePx[] = { 1, 2, 3, 4, 5, 8, 12, 16 }; /* ids 361-368 */

static BOOL HasTrueSelection()
{
    return selectionModel.m_bShow && toolsModel.IsSelection();
}

static BOOL TextShown()
{
    return toolsModel.GetActiveTool() == TOOL_TEXT && ::IsWindowVisible(textEditWindow);
}

static void ApplySize(int px)
{
    switch (toolsModel.GetActiveTool())
    {
    case TOOL_BRUSH:
        toolsModel.SetBrushWidth(px);
        break;
    case TOOL_AIRBRUSH:
        toolsModel.SetAirBrushRadius(px);
        break;
    case TOOL_RUBBER:
        toolsModel.SetRubberRadius(px);
        break;
    default: /* line and shape tools; other tools have no size */
        toolsModel.SetLineWidth(px);
        break;
    }
    toolsModel.NotifyToolSettingsChanged();
}

/* Execute handler: ribbon-only ids act on the models, the rest go to the main window as WM_COMMAND */
static HRESULT OnRibbonExecute(UINT32 id, void *app_context)
{
    HWND hWnd = (HWND)app_context;

    /* Framework commands handled by the ribbon itself */
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return S_OK;

    if (id >= ID_FREESEL && id <= ID_RRECT)
    {
        toolsModel.SetActiveTool((TOOLTYPE)(id - ID_FREESEL + 1));
        return S_OK;
    }
    if (id >= PAINT_PALETTE_FIRST && id < PAINT_PALETTE_FIRST + PAINT_PALETTE_COUNT)
    {
        paletteModel.SetFgColor(g_paintPaletteItems[id - PAINT_PALETTE_FIRST].color);
        return S_OK;
    }
    if (id >= PAINT_PALETTE_ALT_FIRST && id < PAINT_PALETTE_ALT_FIRST + PAINT_PALETTE_COUNT)
    {
        paletteModel.SetBgColor(g_paintPaletteItems[id - PAINT_PALETTE_ALT_FIRST].color);
        return S_OK;
    }
    if (id >= 361 && id <= 368)
    {
        ApplySize(g_sizePx[id - 361]);
        return S_OK;
    }

    switch (id)
    {
    case 380: case 310: case 320: case 340: case 345: case 346: case 360: case 372:
        return S_OK; /* galleries and drop-downs: their items execute */
    case IDM_IMAGECROP:
        id = IDM_CROPSELECTION;
        break;
    case 301: mainWindow.SaveAsFormat(L".png"); return S_OK;
    case 302: mainWindow.SaveAsFormat(L".jpg"); return S_OK;
    case 303: mainWindow.SaveAsFormat(L".bmp"); return S_OK;
    case 304: mainWindow.SaveAsFormat(L".gif"); return S_OK;
    case 305: id = IDM_FILESAVEAS; break;
    case 311: case 312: case 313:
    {
        int n = (id == 311) ? 1 : (id == 312) ? 3 : 2;
        if (selectionModel.m_bShow)
            selectionModel.RotateNTimes90Degrees(n);
        else
            imageModel.RotateNTimes90Degrees(n);
        return S_OK;
    }
    case 314:
        if (selectionModel.m_bShow)
            selectionModel.FlipVertically();
        else
            imageModel.FlipVertically();
        return S_OK;
    case 315:
        if (selectionModel.m_bShow)
            selectionModel.FlipHorizontally();
        else
            imageModel.FlipHorizontally();
        return S_OK;
    case 321: case 322: case 323:
        toolsModel.SetBrushStyle(id == 321 ? BrushStyleSquare : id == 322 ? BrushStyleForeSlash : BrushStyleBackSlash);
        toolsModel.SetActiveTool(TOOL_BRUSH);
        return S_OK;
    case 347: toolsModel.SetShapeStyle(2); return S_OK; /* fill only */
    case 348: toolsModel.SetShapeStyle(0); return S_OK; /* outline only */
    case 350: toolsModel.SetShapeStyle(0); return S_OK;
    case 351: toolsModel.SetShapeStyle(2); return S_OK;
    case 352: toolsModel.SetShapeStyle(1); return S_OK;
    case 370:
    {
        COLORREF c = paletteModel.GetFgColor();
        if (mainWindow.ChooseColor(&c))
            paletteModel.SetFgColor(c);
        return S_OK;
    }
    case 371:
    {
        COLORREF c = paletteModel.GetBgColor();
        if (mainWindow.ChooseColor(&c))
            paletteModel.SetBgColor(c);
        return S_OK;
    }
    case 390: case 391:
        if (toolsModel.GetActiveTool() == TOOL_TEXT && !::IsWindowVisible(fontsDialog))
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDM_FORMATICONBAR, 0), 0);
        return S_OK;
    case 392: case 393:
        toolsModel.SetBackgroundTransparent(id == 393);
        toolsModel.NotifyToolSettingsChanged();
        return S_OK;
    case IDM_BOLD: case IDM_ITALIC: case IDM_UNDERLINE:
    {
        DWORD &v = (id == IDM_BOLD) ? registrySettings.Bold :
                   (id == IDM_ITALIC) ? registrySettings.Italic : registrySettings.Underline;
        v = !v;
        if (fontsDialog.IsWindow())
        {
            HWND hbar = fontsDialog.GetDlgItem(IDD_FONTSTOOLBAR);
            ::SendMessageW(hbar, TB_CHECKBUTTON, id, MAKELPARAM(v, 0));
        }
        toolsModel.NotifyToolChanged();
        return S_OK;
    }
    }

    /* Route other commands to the main window as WM_COMMAND */
    SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    return S_OK;
}

/* Query-state handler: read current state from Paint's model */
static HRESULT OnRibbonQueryState(UINT32 id, LeewayUICommandState *state, void *app_context)
{
    HWND hWnd = (HWND)app_context;
    if (!hWnd || !state)
        return E_INVALIDARG;

    state->enabled = TRUE;
    state->checked = FALSE;
    state->visible = TRUE;
    state->label = NULL;
    state->tooltip = NULL;

    if (id >= ID_FREESEL && id <= ID_RRECT)
    {
        state->checked = ((UINT)toolsModel.GetActiveTool() == id - ID_FREESEL + 1);
        return S_OK;
    }

    BOOL brush = (toolsModel.GetActiveTool() == TOOL_BRUSH);
    switch (id)
    {
    case IDM_EDITUNDO:  state->enabled = mainWindow.CanUndo(); break;
    case IDM_EDITREDO:  state->enabled = mainWindow.CanRedo(); break;
    case IDM_EDITCUT:
    case IDM_EDITCOPY:
    case IDM_EDITDELETESELECTION:
    {
        BOOL textSel = FALSE;
        if (TextShown())
        {
            DWORD s = 0, e = 0;
            textEditWindow.SendMessage(EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
            textSel = (s != e);
            state->enabled = textSel;
        }
        else
        {
            state->enabled = HasTrueSelection();
        }
        break;
    }
    case IDM_EDITPASTE:
    case IDM_EDITPASTEFROM:
        state->enabled = mainWindow.CanPaste();
        break;
    case IDM_IMAGECROP:
        state->enabled = HasTrueSelection();
        break;
    case IDM_IMAGEDRAWOPAQUE:
        state->checked = toolsModel.IsBackgroundTransparent();
        break;
    case IDM_VIEWSTATUSBAR:
        state->checked = ::IsWindowVisible(g_hStatusBar);
        break;
    case IDM_VIEWSHOWGRID:
        state->checked = g_showGrid;
        break;
    case IDM_VIEWSHOWMINIATURE:
        state->checked = ::IsWindowVisible(miniature);
        break;
    case IDM_COLORSMODERNPALETTE:  state->checked = (paletteModel.SelectedPalette() == PAL_MODERN); break;
    case IDM_COLORSOLDPALETTE:     state->checked = (paletteModel.SelectedPalette() == PAL_OLDTYPE); break;
    case IDM_COLORSGRAYSCALE:      state->checked = (paletteModel.SelectedPalette() == PAL_GRAYSCALE); break;
    case IDM_COLORSMONOCHROME:     state->checked = (paletteModel.SelectedPalette() == PAL_MONOCHROME); break;
    case IDM_BOLD:       state->checked = !!registrySettings.Bold; break;
    case IDM_ITALIC:     state->checked = !!registrySettings.Italic; break;
    case IDM_UNDERLINE:  state->checked = !!registrySettings.Underline; break;
    case IDM_VERTICAL:   state->enabled = FALSE; break; /* vertical text is not implemented in Paint */
    case 392: state->checked = !toolsModel.IsBackgroundTransparent(); break;
    case 393: state->checked = toolsModel.IsBackgroundTransparent(); break;
    case 321: state->checked = (brush && toolsModel.GetBrushStyle() == BrushStyleSquare); break;
    case 322: state->checked = (brush && toolsModel.GetBrushStyle() == BrushStyleForeSlash); break;
    case 323: state->checked = (brush && toolsModel.GetBrushStyle() == BrushStyleBackSlash); break;
    default:
        for (size_t i = 0; i < _countof(g_zoomIds); i++)
        {
            if (g_zoomIds[i].id == id)
                state->checked = (toolsModel.GetZoom() == g_zoomIds[i].zoom);
        }
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

    /* Collections: galleries, dropdowns and split menus (paint_ribbon_cmds.h) */
    hr = PaintRibbonSetCollections(*pRibbon);
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

/* Activate/deactivate Text contextual tab (called when tool changes) */
void PaintRibbonSetTextToolActive(ILeewayRibbon *ribbon, BOOL active)
{
    if (ribbon)
        LeewayRibbonSetContext(ribbon, L"IsTextTool", active);
}
