// PROJECT:    Paint Ribbon Standalone Demo for LiberNT
// LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
// PURPOSE:    Standalone native demo: the REAL Paint command table, layout and collections
//             (paint_ribbon_cmds.h, shared with mspaint) in a LeewayUI ribbon + LeewayUI frame,
//             over a small GDI canvas. No precomp.h, no ATL, no ReactOS headers.
// COPYRIGHT:  Copyright 2026 Leeway Open Software
// PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
//
// Build: PATH=/c/msys64/clang64/bin:$PATH sh base/applications/mspaint/ribbon_demo/build.sh
//
// What works: pencil, brush styles (as brush), airbrush, eraser, line, rectangle, ellipse, rounded
// rectangle, fill, color picker; Color 1/Color 2 and the 28-color palette, line sizes, zoom gallery,
// undo/redo, New, flip/rotate 180, gridlines, status bar, the contextual Text tab (shows while the
// Text tool is selected). Selection, text entry, file I/O, resize/skew are not in the demo.

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <vector>
#include <leewayui/frame.h>
#include "../paint_ribbon_cmds.h"

static ILeewayRibbon *g_rib;
static LeewayUIFrame *g_frame;
static HWND g_hwnd;
static RECT g_work;

/* ---- document ---- */
static const int CW = 1024, CH = 640;
static HDC g_dc;
static HBITMAP g_bmp, g_oldBmp;
static std::vector<HBITMAP> g_undo, g_redo;
static UINT g_zoom = 100;

/* ---- tool state ---- */
static UINT32 g_tool = ID_PEN;
static int g_size = 2;
static COLORREF g_c1 = RGB(0, 0, 0), g_c2 = RGB(255, 255, 255);
static int g_target = 1;                 /* which color the palette sets */
static bool g_grid, g_thumb, g_full, g_rulers, g_bold, g_italic, g_underline, g_vertical, g_opaque = true;
static bool g_down;
static POINT g_start, g_last;
static WCHAR g_note[96];

static const int kSizes[] = { 1, 2, 3, 4, 5, 8, 12, 16 };

static void Err(const WCHAR *what, HRESULT hr)
{
    WCHAR m[256];
    swprintf(m, 256, L"%ls failed: 0x%08X", what, (unsigned)hr);
    MessageBoxW(g_hwnd, m, L"Paint Ribbon Demo", MB_OK | MB_ICONERROR);
}

static const WCHAR *ToolName(UINT32 t)
{
    for (UINT i = 0; i < _countof(g_paintCommands); ++i)
        if (g_paintCommands[i].id == t)
            return g_paintCommands[i].label;
    return L"";
}

static void Status()
{
    WCHAR t[256];
    swprintf(t, 256, L"%ls   size %d px   color 1 #%02X%02X%02X   %ls", ToolName(g_tool), g_size,
             GetRValue(g_c1), GetGValue(g_c1), GetBValue(g_c1), g_note);
    LeewayFrameSetStatusText(g_frame, 0, t);
    LeewayFrameSetZoom(g_frame, g_zoom);
}

/* The document bitmap is selected into g_dc for good, so copies are made from g_dc. */
static HBITMAP Snapshot()
{
    HDC b = CreateCompatibleDC(g_dc);
    HBITMAP n = CreateCompatibleBitmap(g_dc, CW, CH);
    HGDIOBJ ob = SelectObject(b, n);
    BitBlt(b, 0, 0, CW, CH, g_dc, 0, 0, SRCCOPY);
    SelectObject(b, ob);
    DeleteDC(b);
    return n;
}

static void Restore(HBITMAP snap)
{
    HDC b = CreateCompatibleDC(g_dc);
    HGDIOBJ ob = SelectObject(b, snap);
    BitBlt(g_dc, 0, 0, CW, CH, b, 0, 0, SRCCOPY);
    SelectObject(b, ob);
    DeleteDC(b);
}

static void PushUndo()
{
    g_undo.push_back(Snapshot());
    if (g_undo.size() > 30)
    {
        DeleteObject(g_undo.front());
        g_undo.erase(g_undo.begin());
    }
    for (HBITMAP h : g_redo)
        DeleteObject(h);
    g_redo.clear();
}

static void Swap(std::vector<HBITMAP> &from, std::vector<HBITMAP> &to)
{
    if (from.empty())
        return;
    HBITMAP snap = from.back();
    from.pop_back();
    to.push_back(Snapshot());
    Restore(snap);
    DeleteObject(snap);
}

static void NewDoc()
{
    RECT r = { 0, 0, CW, CH };
    HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(g_dc, &r, w);
    DeleteObject(w);
}

static POINT ToDoc(int x, int y)
{
    POINT p = { (x - g_work.left - 8) * 100 / (int)g_zoom, (y - g_work.top - 8) * 100 / (int)g_zoom };
    return p;
}

static void Redraw()
{
    InvalidateRect(g_hwnd, &g_work, FALSE);
}

static void SetZoom(UINT z)
{
    g_zoom = z < 12 ? 12 : (z > 800 ? 800 : z);
    Status();
    LeewayRibbonInvalidateCommand(g_rib, 380);
    Redraw();
}

static void SelectTool(UINT32 t)
{
    g_tool = t;
    LeewayRibbonSetContext(g_rib, L"IsTextTool", t == ID_TEXT);   /* the contextual Text tab */
    g_note[0] = 0;
    if (t == ID_TEXT || t == ID_FREESEL || t == ID_RECTSEL || t == ID_ZOOM || t == ID_BEZIER || t == ID_SHAPE)
        wcscpy(g_note, L"(not in the demo)");
    LeewayRibbonInvalidateAll(g_rib);
    Status();
}

/* ---- drawing ---- */
static void Stroke(POINT a, POINT b, COLORREF col, int w)
{
    HPEN pen = CreatePen(PS_SOLID, w, col);
    HGDIOBJ old = SelectObject(g_dc, pen);
    MoveToEx(g_dc, a.x, a.y, NULL);
    LineTo(g_dc, b.x, b.y);
    SetPixel(g_dc, b.x, b.y, col);
    SelectObject(g_dc, old);
    DeleteObject(pen);
}

static void Shape(POINT a, POINT b)
{
    HPEN pen = CreatePen(PS_SOLID, g_size, g_c1);
    HGDIOBJ op = SelectObject(g_dc, pen), ob = SelectObject(g_dc, GetStockObject(NULL_BRUSH));
    if (g_tool == ID_LINE)
    {
        MoveToEx(g_dc, a.x, a.y, NULL);
        LineTo(g_dc, b.x, b.y);
    }
    else if (g_tool == ID_RECT)
        Rectangle(g_dc, a.x, a.y, b.x, b.y);
    else if (g_tool == ID_ELLIPSE)
        Ellipse(g_dc, a.x, a.y, b.x, b.y);
    else if (g_tool == ID_RRECT)
        RoundRect(g_dc, a.x, a.y, b.x, b.y, 24, 24);
    SelectObject(g_dc, op);
    SelectObject(g_dc, ob);
    DeleteObject(pen);
}

static bool Freehand() { return g_tool == ID_PEN || g_tool == ID_BRUSH || g_tool == ID_AIRBRUSH || g_tool == ID_RUBBER; }
static bool Shaped() { return g_tool == ID_LINE || g_tool == ID_RECT || g_tool == ID_ELLIPSE || g_tool == ID_RRECT; }

static void Dab(POINT a, POINT b)
{
    if (g_tool == ID_PEN)
        Stroke(a, b, g_c1, 1);
    else if (g_tool == ID_BRUSH)
        Stroke(a, b, g_c1, g_size * 2);
    else if (g_tool == ID_RUBBER)
        Stroke(a, b, g_c2, g_size * 3);
    else if (g_tool == ID_AIRBRUSH)
        for (int i = 0; i < 12; ++i)
        {
            int r = g_size * 4;
            SetPixel(g_dc, b.x + rand() % (2 * r + 1) - r, b.y + rand() % (2 * r + 1) - r, g_c1);
        }
}

/* ---- commands ---- */
static void Command(UINT id)
{
    if (id >= PAINT_PALETTE_FIRST && id < PAINT_PALETTE_FIRST + PAINT_PALETTE_COUNT)      /* left click: Color 1 */
    {
        g_c1 = g_paintPaletteItems[id - PAINT_PALETTE_FIRST].color;
        Status();
        return;
    }
    if (id >= PAINT_PALETTE_ALT_FIRST && id < PAINT_PALETTE_ALT_FIRST + PAINT_PALETTE_COUNT)   /* right click: Color 2 */
    {
        g_c2 = g_paintPaletteItems[id - PAINT_PALETTE_ALT_FIRST].color;
        Status();
        return;
    }
    if (id >= 361 && id <= 368)
    {
        g_size = kSizes[id - 361];
        Status();
        return;
    }
    if (id >= 321 && id <= 326)          /* brush styles: drawn as the brush */
    {
        SelectTool(ID_BRUSH);
        return;
    }
    if (id >= ID_FREESEL && id <= ID_RRECT)
    {
        SelectTool(id);
        return;
    }
    switch (id)
    {
        case 370: g_target = 1; break;               /* Edit colors changes the selected well */
        case 371: g_target = 2; break;
        case IDM_COLORSEDITPALETTE:
        {
            static COLORREF custom[16];
            CHOOSECOLORW cc = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = g_hwnd;
            cc.lpCustColors = custom;
            cc.rgbResult = g_target == 1 ? g_c1 : g_c2;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
                (g_target == 1 ? g_c1 : g_c2) = cc.rgbResult;
            Status();
            break;
        }
        case IDM_FILENEW: PushUndo(); NewDoc(); Redraw(); break;
        case IDM_EDITUNDO: Swap(g_undo, g_redo); Redraw(); break;
        case IDM_EDITREDO: Swap(g_redo, g_undo); Redraw(); break;
        case IDM_FILEEXIT: DestroyWindow(g_hwnd); break;
        case IDM_HELPINFO:
        case LWUI_HELP:
            MessageBoxW(g_hwnd, L"Paint for LiberNT: ribbon demo.\nThe command table, layout and collections are the real "
                                L"ones (paint_ribbon_cmds.h, shared with mspaint).", L"About Paint", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_VIEWSTATUSBAR:
        case LWUI_STATUSBAR_TOGGLE:
        {
            RECT rc;
            LeewayFrameShowStatusBar(g_frame, !LeewayFrameIsStatusBarShown(g_frame), TRUE);
            GetClientRect(g_hwnd, &rc);
            LeewayUIFrameRects fr;
            LeewayFrameLayout(g_frame, &rc, &fr);
            g_work = fr.work;
            Redraw();
            break;
        }
        case IDM_VIEWSHOWGRID: g_grid = !g_grid; Redraw(); break;
        case IDM_VIEWSHOWMINIATURE: g_thumb = !g_thumb; break;
        case IDM_VIEWFULLSCREEN: g_full = !g_full; break;
        case 381: g_rulers = !g_rulers; break;
        case IDM_BOLD: g_bold = !g_bold; break;
        case IDM_ITALIC: g_italic = !g_italic; break;
        case IDM_UNDERLINE: g_underline = !g_underline; break;
        case IDM_VERTICAL: g_vertical = !g_vertical; break;
        case 392: g_opaque = true; break;
        case 393: g_opaque = false; break;
        case IDM_VIEWZOOM125: SetZoom(12); break;
        case IDM_VIEWZOOM25: SetZoom(25); break;
        case IDM_VIEWZOOM50: SetZoom(50); break;
        case IDM_VIEWZOOM100: SetZoom(100); break;
        case IDM_VIEWZOOM200: SetZoom(200); break;
        case IDM_VIEWZOOM400: SetZoom(400); break;
        case IDM_VIEWZOOM800: SetZoom(800); break;
        case IDM_CTRL_PLUS: SetZoom(g_zoom * 2); break;
        case IDM_CTRL_MINUS: SetZoom(g_zoom / 2); break;
        case 313: case 314: case 315:
        {
            PushUndo();
            HBITMAP c = Snapshot();
            HDC s = CreateCompatibleDC(g_dc);
            HGDIOBJ o = SelectObject(s, c);
            bool v = (id == 313 || id == 314), h = (id == 313 || id == 315);   /* 313: 180, 314: flip V, 315: flip H */
            StretchBlt(g_dc, h ? CW - 1 : 0, v ? CH - 1 : 0, h ? -CW : CW, v ? -CH : CH, s, 0, 0, CW, CH, SRCCOPY);
            SelectObject(s, o);
            DeleteDC(s);
            DeleteObject(c);
            Redraw();
            break;
        }
        case 311: case 312: wcscpy(g_note, L"(90 degree rotation not in the demo)"); Status(); break;
        default:
            swprintf(g_note, 96, L"(command %u: not in the demo)", id);
            Status();
            break;
    }
    LeewayRibbonInvalidateAll(g_rib);
}

static HRESULT WINAPI OnExecute(UINT32 id, void *)
{
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE || id == LWUI_RIBBON_MINIMIZE)
        return S_OK;
    Command(id);
    return S_OK;
}

static HRESULT WINAPI OnQueryState(UINT32 id, LeewayUICommandState *st, void *)
{
    static WCHAR zoomLabel[16];
    st->enabled = TRUE;
    st->visible = TRUE;
    st->checked = FALSE;
    if (id >= ID_FREESEL && id <= ID_RRECT)
        st->checked = (id == g_tool);
    switch (id)
    {
        case IDM_EDITUNDO: st->enabled = !g_undo.empty(); break;
        case IDM_EDITREDO: st->enabled = !g_redo.empty(); break;
        case IDM_VIEWSHOWGRID: st->checked = g_grid; break;
        case IDM_VIEWSHOWMINIATURE: st->checked = g_thumb; break;
        case IDM_VIEWFULLSCREEN: st->checked = g_full; break;
        case IDM_VIEWSTATUSBAR: st->checked = LeewayFrameIsStatusBarShown(g_frame); break;
        case 381: st->checked = g_rulers; break;
        case IDM_BOLD: st->checked = g_bold; break;
        case IDM_ITALIC: st->checked = g_italic; break;
        case IDM_UNDERLINE: st->checked = g_underline; break;
        case IDM_VERTICAL: st->checked = g_vertical; break;
        case 392: st->checked = g_opaque; break;
        case 393: st->checked = !g_opaque; break;
        case IDM_FILEFROMSCANNERORCAMERA: st->enabled = FALSE; break;
        case 370: st->color = g_c1; break;          /* the colour wells show the colors */
        case 371: st->color = g_c2; break;
        case 380:
            swprintf(zoomLabel, 16, L"%u %%", g_zoom);
            st->label = zoomLabel;
            break;
        default: break;
    }
    return S_OK;
}

/* ---- window ---- */
static void Layout()
{
    RECT rc;
    LeewayUIFrameRects fr;
    GetClientRect(g_hwnd, &rc);
    LeewayFrameLayout(g_frame, &rc, &fr);
    g_work = fr.work;
    Redraw();
}

static void OnFrameLayout(const LeewayUIFrameRects *, void *) { Layout(); }

static void Paint(HDC dc)
{
    int w = g_work.right - g_work.left, h = g_work.bottom - g_work.top;
    if (w <= 0 || h <= 0)
        return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r = { 0, 0, w, h };
    FillRect(mem, &r, GetSysColorBrush(COLOR_APPWORKSPACE));
    int dw = CW * (int)g_zoom / 100, dh = CH * (int)g_zoom / 100;
    SetStretchBltMode(mem, g_zoom < 100 ? HALFTONE : COLORONCOLOR);
    StretchBlt(mem, 8, 8, dw, dh, g_dc, 0, 0, CW, CH, SRCCOPY);
    if (g_grid && g_zoom >= 400)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HGDIOBJ op = SelectObject(mem, pen);
        int step = (int)g_zoom / 100;
        for (int x = 0; x <= dw; x += step) { MoveToEx(mem, 8 + x, 8, NULL); LineTo(mem, 8 + x, 8 + dh); }
        for (int y = 0; y <= dh; y += step) { MoveToEx(mem, 8, 8 + y, NULL); LineTo(mem, 8 + dw, 8 + y); }
        SelectObject(mem, op);
        DeleteObject(pen);
    }
    BitBlt(dc, g_work.left, g_work.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT lr = 0;
    if (g_frame && LeewayFrameHandleMessage(g_frame, msg, wp, lp, &lr))
        return lr;

    switch (msg)
    {
        case WM_SIZE:
            if (g_frame)
                Layout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            POINT p = ToDoc(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (p.x < 0 || p.y < 0 || p.x >= CW || p.y >= CH)
                return 0;
            if (g_tool == ID_COLOR)
            {
                g_c1 = GetPixel(g_dc, p.x, p.y);
                Status();
                return 0;
            }
            if (g_tool == ID_FILL)
            {
                PushUndo();
                HBRUSH b = CreateSolidBrush(g_c1);
                HGDIOBJ o = SelectObject(g_dc, b);
                ExtFloodFill(g_dc, p.x, p.y, GetPixel(g_dc, p.x, p.y), FLOODFILLSURFACE);
                SelectObject(g_dc, o);
                DeleteObject(b);
                Redraw();
                LeewayRibbonInvalidateCommand(g_rib, IDM_EDITUNDO);
                return 0;
            }
            if (!Freehand() && !Shaped())
                return 0;
            PushUndo();
            LeewayRibbonInvalidateCommand(g_rib, IDM_EDITUNDO);
            g_down = true;
            g_start = g_last = p;
            SetCapture(hwnd);
            if (Freehand())
            {
                Dab(p, p);
                Redraw();
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_down && Freehand())
            {
                POINT p = ToDoc(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                Dab(g_last, p);
                g_last = p;
                Redraw();
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_down)
            {
                g_down = false;
                ReleaseCapture();
                if (Shaped())
                {
                    Shape(g_start, ToDoc(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
                    Redraw();
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
                SetZoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? g_zoom * 5 / 4 : g_zoom * 4 / 5);
            return 0;
        case WM_DESTROY:
            LeewayFrameDestroy(g_frame);      /* borrowed ribbon: ours to destroy */
            g_frame = NULL;
            LeewayRibbonDestroy(g_rib);
            g_rib = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PaintRibbonDemo";
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    if (!RegisterClassW(&wc))
    {
        Err(L"RegisterClassW", HRESULT_FROM_WIN32(GetLastError()));
        return 1;
    }
    HDC scr = GetDC(NULL);
    g_dc = CreateCompatibleDC(scr);
    g_bmp = CreateCompatibleBitmap(scr, CW, CH);
    ReleaseDC(NULL, scr);
    g_oldBmp = (HBITMAP)SelectObject(g_dc, g_bmp);
    NewDoc();

    g_hwnd = CreateWindowW(wc.lpszClassName, L"Paint", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, NULL, NULL, inst, NULL);
    if (!g_hwnd)
    {
        Err(L"CreateWindowW", HRESULT_FROM_WIN32(GetLastError()));
        return 1;
    }

    HRESULT hr = LeewayRibbonCreate(g_hwnd, g_paintCommands, _countof(g_paintCommands), OnExecute, OnQueryState, NULL, &g_rib);
    if (FAILED(hr))
    {
        Err(L"LeewayRibbonCreate", hr);
        return 1;
    }
    hr = LeewayRibbonSetLayout(g_rib, &g_paintLayout);
    if (FAILED(hr))
    {
        Err(L"LeewayRibbonSetLayout", hr);
        return 1;
    }
    hr = PaintRibbonSetCollections(g_rib);
    if (FAILED(hr))
        Err(L"LeewayRibbonSetCollection", hr);       /* not fatal: the ribbon works without some menus */

    static const UINT32 zl[] = { 12, 25, 50, 100, 200, 400, 800 };
    LeewayUIFrameDesc d = {};
    d.struct_size = sizeof(d);
    d.owner = g_hwnd;
    d.instance = inst;
    d.app_name = L"Paint";
    d.flags = LWUI_FRAME_STATUSBAR;
    d.default_chrome = LWUI_CHROME_RIBBON;
    d.ribbon = g_rib;
    d.zoom_levels = zl;
    d.zoom_level_count = 0;
    d.on_layout = OnFrameLayout;
    hr = LeewayFrameCreate(&d, &g_frame);
    if (FAILED(hr))
    {
        Err(L"LeewayFrameCreate", hr);
        return 1;
    }
    Layout();
    Status();

    ShowWindow(g_hwnd, show ? show : SW_SHOW);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (g_rib && LeewayRibbonPreTranslateMessage(g_rib, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
