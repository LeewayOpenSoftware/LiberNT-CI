/*
 * PROJECT:    PAINT for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Paint ribbon handlers and integration
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 * PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include "precomp.h"
#include "paint_ribbon.h"
#include "paint_ribbon_ids.h"
#include <vector>
#include <deque>
#include "../../../sdk/include/leewayui/aureline_icons.h"

extern CMainWindow mainWindow;
extern BOOL g_showGrid;

static ILeewayRibbon *s_ribbon;

#include "paint_ribbon_strings.h"

/* Label text comes from the resource string tables (en-US, es-ES, ...); the literal is the fallback */
static const WCHAR *PaintTr(const WCHAR *en)
{
    static std::deque<std::wstring> s_cache; /* deque: element addresses stay valid */
    if (!en)
        return en;
    for (size_t i = 0; i < _countof(g_ribbonStrings); i++)
    {
        if (wcscmp(g_ribbonStrings[i].en, en) != 0)
            continue;
        WCHAR buf[256];
        if (::LoadStringW(g_hinstExe, g_ribbonStrings[i].id, buf, _countof(buf)) > 0)
        {
            s_cache.push_back(buf);
            return s_cache.back().c_str();
        }
        break;
    }
    return en;
}

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
static BOOL HandleLocal(UINT32 id, HWND hWnd)
{

    /* Framework commands handled by the ribbon itself */
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return TRUE;

    if (id >= ID_FREESEL && id <= ID_RRECT)
    {
        toolsModel.SetActiveTool((TOOLTYPE)(id - ID_FREESEL + 1));
        return TRUE;
    }
    if ((id >= PAINT_PALETTE_FIRST && id < PAINT_PALETTE_FIRST + PAINT_PALETTE_COUNT) ||
        (id >= PAINT_PALETTE_ALT_FIRST && id < PAINT_PALETTE_ALT_FIRST + PAINT_PALETTE_COUNT))
    {
        BOOL bAlt = (id >= PAINT_PALETTE_ALT_FIRST);
        UINT n = id - (bAlt ? PAINT_PALETTE_ALT_FIRST : PAINT_PALETTE_FIRST);
        if (n >= PAINT_PALETTE_STD && !g_paintCustomSet[n - PAINT_PALETTE_STD])
        {
            /* An empty custom slot: define it first */
            COLORREF c = bAlt ? paletteModel.GetBgColor() : paletteModel.GetFgColor();
            if (!mainWindow.ChooseColor(&c))
                return TRUE;
            g_paintCustomSet[n - PAINT_PALETTE_STD] = TRUE;
            g_paintPaletteItems[n].color = c;
            if (s_ribbon)
                PaintSetCollTr(s_ribbon, 372, g_paintPaletteItems, _countof(g_paintPaletteItems),
                               LWUI_COLLECTION_COLORS);
        }
        if (bAlt)
            paletteModel.SetBgColor(g_paintPaletteItems[n].color);
        else
            paletteModel.SetFgColor(g_paintPaletteItems[n].color);
        return TRUE;
    }
    if (id >= 361 && id <= 368)
    {
        ApplySize(g_sizePx[id - 361]);
        return TRUE;
    }

    switch (id)
    {
    case 380: case 310: case 320: case 340: case 345: case 346: case 360: case 372:
        return TRUE; /* galleries and drop-downs: their items execute */
    case IDM_IMAGECROP:
        SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDM_CROPSELECTION, 0), 0);
        return TRUE;
    case 301: mainWindow.SaveAsFormat(L".png"); return TRUE;
    case 302: mainWindow.SaveAsFormat(L".jpg"); return TRUE;
    case 303: mainWindow.SaveAsFormat(L".bmp"); return TRUE;
    case 304: mainWindow.SaveAsFormat(L".gif"); return TRUE;
    case 305:
        SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDM_FILESAVEAS, 0), 0);
        return TRUE;
    case 311: case 312: case 313:
    {
        int n = (id == 311) ? 1 : (id == 312) ? 3 : 2;
        if (selectionModel.m_bShow)
            selectionModel.RotateNTimes90Degrees(n);
        else
            imageModel.RotateNTimes90Degrees(n);
        return TRUE;
    }
    case 314:
        if (selectionModel.m_bShow)
            selectionModel.FlipVertically();
        else
            imageModel.FlipVertically();
        return TRUE;
    case 315:
        if (selectionModel.m_bShow)
            selectionModel.FlipHorizontally();
        else
            imageModel.FlipHorizontally();
        return TRUE;
    case 321: case 322: case 323:
        toolsModel.SetBrushStyle(id == 321 ? BrushStyleSquare : id == 322 ? BrushStyleForeSlash : BrushStyleBackSlash);
        toolsModel.SetActiveTool(TOOL_BRUSH);
        return TRUE;
    case 347: toolsModel.SetShapeStyle(2); return TRUE; /* fill only */
    case 348: toolsModel.SetShapeStyle(0); return TRUE; /* outline only */
    case 350: toolsModel.SetShapeStyle(0); return TRUE;
    case 351: toolsModel.SetShapeStyle(2); return TRUE;
    case 352: toolsModel.SetShapeStyle(1); return TRUE;
    case 370:
    {
        COLORREF c = paletteModel.GetFgColor();
        if (mainWindow.ChooseColor(&c))
            paletteModel.SetFgColor(c);
        return TRUE;
    }
    case 371:
    {
        COLORREF c = paletteModel.GetBgColor();
        if (mainWindow.ChooseColor(&c))
            paletteModel.SetBgColor(c);
        return TRUE;
    }
    case 390: case 391:
        if (toolsModel.GetActiveTool() == TOOL_TEXT && !::IsWindowVisible(fontsDialog))
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDM_FORMATICONBAR, 0), 0);
        return TRUE;
    case 392: case 393:
        toolsModel.SetBackgroundTransparent(id == 393);
        toolsModel.NotifyToolSettingsChanged();
        return TRUE;
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
        return TRUE;
    }
    }

    return FALSE;
}

/* Called by CMainWindow::OnCommand first: ribbon-only ids (menu, accelerator or ribbon) act here */
BOOL PaintRibbonHandleCommand(UINT32 id)
{
    return HandleLocal(id, mainWindow);
}

/* Execute handler: ribbon-only ids act on the models, the rest go to the main window as WM_COMMAND */
static HRESULT OnRibbonExecute(UINT32 id, void *app_context)
{
    HWND hWnd = (HWND)app_context;
    if (!HandleLocal(id, hWnd))
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
    if (id == 370 || id == 371)
    {
        state->color = (id == 370) ? paletteModel.GetFgColor() : paletteModel.GetBgColor();
        return S_OK;
    }
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

/* Shapes gallery: each cell draws its own outline (ids are the tool commands) */
static HRESULT PaintGalleryDraw(UINT32 cmd, UINT32 item, HDC hdc, const RECT *cell, COLORREF fg, UINT32 dpi, void *ctx)
{
    if (item < ID_LINE || item > ID_RRECT) /* the strip passes command_id 0 */
        return E_NOTIMPL;
    int w = dpi ? (int)((dpi + 48) / 96) : 1;
    if (w < 1)
        w = 1;
    HPEN pen = CreatePen(PS_SOLID, w, fg);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RECT c = *cell;
    int m = (c.right - c.left) / 5;
    InflateRect(&c, -m, -m);
    int l = c.left, t = c.top, r = c.right, b = c.bottom, cx = (l + r) / 2;
    switch (item)
    {
    case ID_LINE:    MoveToEx(hdc, l, b, NULL); LineTo(hdc, r, t); break;
    case ID_BEZIER:  { POINT p[4] = { { l, b }, { l, t }, { cx, b }, { r, t } }; PolyBezier(hdc, p, 4); } break;
    case ID_RECT:    Rectangle(hdc, l, t, r, b); break;
    case ID_SHAPE:   { POINT p[5] = { { l, b }, { l, t + (b - t) / 3 }, { cx, t }, { r, t + (b - t) / 3 }, { r, b } }; Polygon(hdc, p, 5); } break;
    case ID_ELLIPSE: Ellipse(hdc, l, t, r, b); break;
    case ID_RRECT:   RoundRect(hdc, l, t, r, b, (r - l) / 3, (b - t) / 3); break;
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    return S_OK;
}

/* Initialize Paint ribbon: called from CMainWindow::OnCreate */
HRESULT PaintRibbonInitialize(HWND hwndMain, ILeewayRibbon **pRibbon)
{
    if (!hwndMain || !pRibbon)
        return E_INVALIDARG;

    g_paintTr = PaintTr;

    /* Translated copies of the command table and the layout (the ribbon copies what it is given) */
    std::vector<std::wstring> keep;
    keep.reserve(1024);
    std::vector<LeewayUICommand> cmds(g_paintCommands, g_paintCommands + _countof(g_paintCommands));
    for (size_t i = 0; i < cmds.size(); i++)
    {
        cmds[i].label = PaintTr(cmds[i].label);
        cmds[i].tooltip = PaintTr(cmds[i].tooltip);
    }
    LeewayUICommand appBtn = { LWUI_APPMENU, PaintTr(L"File"), NULL, NULL, 0, TRUE, FALSE, TRUE };
    cmds.push_back(appBtn);

    std::vector<std::vector<LeewayUIGroup> > groupSets(_countof(g_paintTabs));
    std::vector<LeewayUITab> tabs(g_paintTabs, g_paintTabs + _countof(g_paintTabs));
    for (size_t t = 0; t < tabs.size(); t++)
    {
        groupSets[t].assign(tabs[t].groups, tabs[t].groups + tabs[t].group_count);
        for (size_t g = 0; g < groupSets[t].size(); g++)
            groupSets[t][g].label = PaintTr(groupSets[t][g].label);
        tabs[t].groups = groupSets[t].data();
        tabs[t].label = PaintTr(tabs[t].label);
    }
    LeewayUILayout layout = g_paintLayout;
    layout.tabs = tabs.data();

    /* Create ribbon with Paint's command table */
    HRESULT hr = LeewayRibbonCreate(hwndMain,
                                    cmds.data(), (UINT32)cmds.size(),
                                    OnRibbonExecute,
                                    OnRibbonQueryState,
                                    (void *)hwndMain,
                                    pRibbon);
    if (FAILED(hr))
        return hr;

    /* Set the layout */
    hr = LeewayRibbonSetLayout(*pRibbon, &layout);
    if (FAILED(hr))
    {
        LeewayRibbonDestroy(*pRibbon);
        *pRibbon = NULL;
        return hr;
    }

    /* Aureline icons from <exe dir>\icons; a missing file leaves the letter placeholder */
    static WCHAR s_iconRoot[MAX_PATH];
    static LeewayAurelineConfig s_iconCfg;
    if (GetModuleFileNameW(NULL, s_iconRoot, _countof(s_iconRoot)))
    {
        WCHAR *pSlash = wcsrchr(s_iconRoot, L'\\');
        if (pSlash && (size_t)(pSlash - s_iconRoot) + 7 < _countof(s_iconRoot))
        {
            wcscpy(pSlash + 1, L"icons");
            s_iconCfg.root = s_iconRoot;
            s_iconCfg.mono_fallback_to_colour = TRUE;
            LeewayRibbonUseAurelineIcons(*pRibbon, &s_iconCfg);
        }
    }

    s_ribbon = *pRibbon;
    LeewayRibbonSetGalleryDrawCallback(*pRibbon, PaintGalleryDraw, NULL);

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
    s_ribbon = NULL;
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
