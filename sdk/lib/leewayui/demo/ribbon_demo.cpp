/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Standalone Win11 host harness for the LeewayUI ribbon (ribbon-f2
 *              per-milestone test app). No bison, no CMake: a direct g++/MinGW
 *              build linking ribbon.cpp + theme.cpp + dpi.cpp (see build-win11.sh).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * It builds a small WordPad-shaped layout (Home: Clipboard/Font/Paragraph, View)
 * with the Font group's Text-colour and Highlight split buttons backed by a
 * colour-grid dropdown (G15), plus an application menu, so each ribbon-f2
 * milestone can be driven and screenshotted on Win11. `--shot <file.bmp>` opens a
 * surface (app menu or a colour dropdown) and saves the window to a BMP, then
 * exits, for automated evidence. Clean room: no Microsoft code or resources.
 */
#include <windows.h>
#include <leewayui/ribbon.h>
#include <leewayui/ribbon_icons.h>
#include <leewayui/aureline_icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- command ids (WordPad-shaped, arbitrary for the demo) ---- */
enum {
    CMD_NEW = 1001, CMD_OPEN = 1002, CMD_SAVE = 1003, CMD_SAVEAS = 1004, CMD_PRINT = 1010,
    CMD_PASTE = 1312, CMD_CUT = 1311, CMD_COPY = 1310,
    CMD_BOLD = 1401, CMD_ITALIC = 1402, CMD_UNDER = 1404,
    CMD_TEXTCOLOR = 1403, CMD_HIGHLIGHT = 1913,
    CMD_ALIGNL = 1501, CMD_ALIGNC = 1502, CMD_ALIGNR = 1503,
    CMD_ZOOMIN = 1601, CMD_ZOOMOUT = 1602, CMD_ZOOM100 = 1603,
    CMD_SHAPES = 340,
    SH_LINE = 610, SH_CURVE = 611, SH_RECT = 612, SH_POLY = 613, SH_OVAL = 614, SH_RROUND = 615,
    /* text colour cells (fixed palette ids) */
    COL_BLACK = 1800, COL_AUTOMATIC = 1816,
    /* highlight colour cells */
    HL_BASE = 1920,
};

static COLORREF g_textColor = RGB(0, 0, 0);
static COLORREF g_highlight = RGB(255, 255, 0);
static BOOL g_bold = FALSE, g_italic = FALSE;
static WCHAR g_status[256] = L"ready";

/* 16 standard colours for the text-colour grid. */
static const COLORREF kPalette[16] = {
    RGB(0,0,0), RGB(128,128,128), RGB(192,0,0), RGB(255,0,0),
    RGB(255,192,0), RGB(255,255,0), RGB(146,208,80), RGB(0,176,80),
    RGB(0,176,240), RGB(0,112,192), RGB(0,32,96), RGB(112,48,160),
    RGB(255,255,255), RGB(191,191,191), RGB(127,127,127), RGB(89,89,89),
};

static HRESULT CALLBACK OnGalleryDraw(UINT32 cmd, UINT32 item, HDC hdc, const RECT *cell, COLORREF fg, UINT32 dpi, void *ctx)
{
    (void)cmd; (void)ctx;
    int w = dpi ? (int)((dpi + 48) / 96) : 1; if (w < 1) w = 1;
    HPEN pen = CreatePen(PS_SOLID, w, fg); HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RECT c = *cell; int m = (c.right - c.left) / 5; InflateRect(&c, -m, -m);
    int l=c.left,t=c.top,rr=c.right,b=c.bottom,cx=(l+rr)/2;
    switch (item) {
    case SH_LINE: MoveToEx(hdc,l,b,NULL); LineTo(hdc,rr,t); break;
    case SH_CURVE: { POINT p[4]={{l,b},{(l+cx)/1,t},{cx,b},{rr,t}}; PolyBezier(hdc,p,4); } break;
    case SH_RECT: Rectangle(hdc,l,t,rr,b); break;
    case SH_POLY: { POINT p[3]={{cx,t},{l,b},{rr,b}}; Polygon(hdc,p,3); } break;
    case SH_OVAL: Ellipse(hdc,l,t,rr,b); break;
    case SH_RROUND: RoundRect(hdc,l,t,rr,b,(rr-l)/3,(b-t)/3); break;
    default: SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(pen); return E_NOTIMPL;
    }
    SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(pen); return S_OK;
}

static void CALLBACK OnRecentOpen(const WCHAR *path, void *ctx)
{
    (void)ctx;
    swprintf(g_status, 256, L"open recent: %s", path);
    OutputDebugStringW(g_status);
}

static HRESULT CALLBACK OnExecute(UINT32 id, void *ctx)
{
    (void)ctx;
    if (id >= COL_BLACK && id < COL_BLACK + 16) {
        g_textColor = kPalette[id - COL_BLACK];
        swprintf(g_status, 256, L"text colour -> #%06lX", (unsigned long)(g_textColor & 0xFFFFFF));
    } else if (id == COL_AUTOMATIC) {
        g_textColor = RGB(0, 0, 0);
        wcscpy(g_status, L"text colour -> Automatic");
    } else if (id >= HL_BASE && id < HL_BASE + 16) {
        g_highlight = kPalette[id - HL_BASE];
        swprintf(g_status, 256, L"highlight -> #%06lX", (unsigned long)(g_highlight & 0xFFFFFF));
    } else if (id == CMD_BOLD) {
        g_bold = !g_bold; wcscpy(g_status, g_bold ? L"bold on" : L"bold off");
    } else if (id == CMD_ITALIC) {
        g_italic = !g_italic; wcscpy(g_status, g_italic ? L"italic on" : L"italic off");
    } else {
        swprintf(g_status, 256, L"execute id=%u", id);
    }
    OutputDebugStringW(g_status);
    return S_OK;
}

static HRESULT CALLBACK OnState(UINT32 id, LeewayUICommandState *st, void *ctx)
{
    (void)ctx;
    st->enabled = TRUE;
    if (id == CMD_TEXTCOLOR) st->color = g_textColor;
    else if (id == CMD_HIGHLIGHT) st->color = g_highlight;
    else if (id == CMD_BOLD) st->checked = g_bold;
    else if (id == CMD_ITALIC) st->checked = g_italic;
    return S_OK;
}

#define L0 LWUI_CONTROL_BUTTON_LARGE
#define S0 LWUI_CONTROL_BUTTON_SMALL
#define TS LWUI_CONTROL_TOGGLE_SMALL
#define SPS LWUI_CONTROL_SPLIT_SMALL
#define SPSI LWUI_CONTROL_SPLIT_SMALL_ICON

static const LeewayUICommand kTable[] = {
    { CMD_NEW,    L"New",    0,0, LWUI_AUR_COMMON_NEW, 1,0,1 },
    { CMD_OPEN,   L"Open",   0,0, LWUI_AUR_COMMON_OPEN, 1,0,1 },
    { CMD_SAVE,   L"Save",   0,0, LWUI_AUR_COMMON_SAVE, 1,0,1 },
    { CMD_SAVEAS, L"Save As",0,0, LWUI_AUR_COMMON_SAVEAS, 1,0,1 },
    { CMD_PRINT,  L"Print",  0,0, LWUI_AUR_COMMON_PRINT, 1,0,1 },
    { CMD_PASTE,  L"Paste",  0,L"V", LWUI_AUR_COMMON_PASTE, 1,0,1 },
    { CMD_CUT,    L"Cut",    0,L"X", LWUI_AUR_COMMON_CUT, 1,0,1 },
    { CMD_COPY,   L"Copy",   0,L"C", LWUI_AUR_COMMON_COPY, 1,0,1 },
    { CMD_BOLD,   L"Bold",   0,L"1", LWUI_ICON_NONE, 1,0,1 },
    { CMD_ITALIC, L"Italic", 0,L"2", LWUI_ICON_NONE, 1,0,1 },
    { CMD_UNDER,  L"Underline",0,L"3", LWUI_ICON_NONE, 1,0,1 },
    { CMD_TEXTCOLOR, L"Text colour", 0,L"FC", LWUI_AUR_WORDPAD_TEXT_COLOUR, 1,0,1 },
    { CMD_HIGHLIGHT, L"Highlight",  0,L"H",  LWUI_AUR_WORDPAD_HIGHLIGHT, 1,0,1 },
    { CMD_ALIGNL, L"Left",   0,L"AL", LWUI_AUR_WORDPAD_ALIGN_LEFT, 1,0,1 },
    { CMD_ALIGNC, L"Center", 0,L"AC", LWUI_AUR_WORDPAD_ALIGN_CENTRE, 1,0,1 },
    { CMD_ALIGNR, L"Right",  0,L"AR", LWUI_AUR_WORDPAD_ALIGN_RIGHT, 1,0,1 },
    { CMD_ZOOMIN, L"Zoom In",0,L"I", LWUI_ICON_NONE, 1,0,1 },
    { CMD_ZOOMOUT,L"Zoom Out",0,L"O",LWUI_ICON_NONE, 1,0,1 },
    { CMD_ZOOM100,L"100%",   0,L"J", LWUI_ICON_NONE, 1,0,1 },
    { CMD_SHAPES, L"Shapes", 0,L"SH", LWUI_ICON_NONE, 1,0,1 },
};

static const LeewayUIPlacement kClip[] = {
    { CMD_PASTE, SPS, 0, 0 }, { CMD_CUT, S0, 0, 0 }, { CMD_COPY, S0, 0, 0 },
};
static const LeewayUIPlacement kFont[] = {
    { CMD_BOLD, TS, 0, 0 }, { CMD_ITALIC, TS, 0, 0 }, { CMD_UNDER, TS, 0, 0 },
    { CMD_TEXTCOLOR, SPS, 0, 0 }, { CMD_HIGHLIGHT, SPSI, 0, 0 },
};
static const LeewayUIPlacement kPara[] = {
    { CMD_ALIGNL, TS, 0, 0 }, { CMD_ALIGNC, TS, 0, 0 }, { CMD_ALIGNR, TS, 0, 0 },
};
static const LeewayUIPlacement kShapes[] = { { CMD_SHAPES, LWUI_CONTROL_GALLERY_STRIP, 0, 120 } };
static const LeewayUIPlacement kZoom[] = {
    { CMD_ZOOMIN, S0, 0, 0 }, { CMD_ZOOMOUT, S0, 0, 0 }, { CMD_ZOOM100, S0, 0, 0 },
};
static const LeewayUIGroup kHomeGroups[] = {
    { L"Clipboard", 0, 0, 0, 3, kClip },
    { L"Font",      0, 0, 0, 5, kFont, 2 },
    { L"Paragraph", 0, 0, 0, 3, kPara },
    { L"Shapes", 0, 0, 0, 1, kShapes },
};
static const LeewayUIGroup kViewGroups[] = {
    { L"Zoom", 0, 0, 0, 3, kZoom },
};
static const LeewayUITab kTabs[] = {
    { 1, L"Home", L"H", 0, 4, kHomeGroups },
    { 2, L"View", L"V", 0, 1, kViewGroups },
};
static const UINT32 kAppMenu[] = { CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_PRINT };
static const UINT32 kQat[] = { CMD_SAVE, CMD_OPEN };
static const LeewayUILayout kLayout = {
    L"RibbonDemo", 2, kTabs, 5, kAppMenu, 2, kQat, 0,
};

static ILeewayRibbon *g_ribbon;

/* colour-grid collections (G15): 16 swatches + an Automatic text row. */
static void SetColourCollections(ILeewayRibbon *r)
{
    LeewayUICollectionItem text[17];
    for (int i = 0; i < 16; i++) {
        text[i].id = COL_BLACK + i; text[i].label = 0; text[i].icon_id = 0;
        text[i].color = kPalette[i]; text[i].alt_id = 0;
    }
    text[16].id = COL_AUTOMATIC; text[16].label = L"Automatic"; text[16].icon_id = 0;
    text[16].color = LWUI_COLOR_NONE; text[16].alt_id = 0;
    LeewayRibbonSetCollection(r, CMD_TEXTCOLOR, text, 17,
                              LWUI_COLLECTION_COLORS | LWUI_COLLECTION_LAST_CHOICE_PRIMARY);

    LeewayUICollectionItem hl[16];
    for (int i = 0; i < 16; i++) {
        hl[i].id = HL_BASE + i; hl[i].label = 0; hl[i].icon_id = 0;
        hl[i].color = kPalette[i]; hl[i].alt_id = 0;
    }
    LeewayRibbonSetCollection(r, CMD_HIGHLIGHT, hl, 16,
                              LWUI_COLLECTION_COLORS | LWUI_COLLECTION_LAST_CHOICE_PRIMARY);
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
static void SaveWindowBmp(HWND hwnd, const WCHAR *path)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC dc = GetDC(hwnd), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    /* PrintWindow renders the window AND its child windows (the ribbon) into mem;
     * a plain BitBlt of the parent DC would miss child windows. */
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT))
        BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1;
    ih.biBitCount = 24; ih.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) & ~3); int img = stride * h;
    BYTE *bits = (BYTE *)malloc(img);
    BITMAPINFO bi = {}; bi.bmiHeader = ih;
    GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + img;
    FILE *f = _wfopen(path, L"wb");
    if (f) { fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(bits, img, 1, f); fclose(f); }
    free(bits); SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(hwnd, dc);
}

static void SaveHwndBmp(HWND hwnd, const WCHAR *path)
{
    RECT wr; GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return;
    HDC dc = GetDC(hwnd), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) PrintWindow(hwnd, mem, 0);
    BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) & ~3); int img = stride * h;
    BYTE *bits = (BYTE *)malloc(img); BITMAPINFO bi = {}; bi.bmiHeader = ih;
    GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + img;
    FILE *f = _wfopen(path, L"wb");
    if (f) { fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(bits, img, 1, f); fclose(f); }
    free(bits); SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(hwnd, dc);
}

static void SaveScreenRectBmp(HWND hwnd, const WCHAR *path)
{
    RECT wr; GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    HDC sdc = GetDC(NULL), mem = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, sdc, wr.left, wr.top, SRCCOPY);
    BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) & ~3); int img = stride * h;
    BYTE *bits = (BYTE *)malloc(img); BITMAPINFO bi = {}; bi.bmiHeader = ih;
    GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + img;
    FILE *f = _wfopen(path, L"wb");
    if (f) { fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(bits, img, 1, f); fclose(f); }
    free(bits); SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, sdc);
}

static int g_themePref = -1;
static const WCHAR *g_shot;
static const WCHAR *g_openSurface; /* "appmenu" | "textcolor" | NULL */
static int g_width = 1000;          /* --width <px>: narrow it to force the band overflow chevron */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        LeewayRibbonCreate(hwnd, kTable, (UINT32)(sizeof kTable / sizeof kTable[0]),
                           OnExecute, OnState, 0, &g_ribbon);
        LeewayRibbonSetLayout(g_ribbon, &kLayout);
        SetColourCollections(g_ribbon);
        static LeewayAurelineConfig s_aur = { L"../icons", TRUE };
        LeewayRibbonUseAurelineIcons(g_ribbon, &s_aur);
        LeewayRibbonSetRecentProvider(g_ribbon, OnRecentOpen, NULL, 8);
        static const LeewayUICollectionItem shapes[] = {
            { SH_LINE,0,0,LWUI_COLOR_NONE,0 },{ SH_CURVE,0,0,LWUI_COLOR_NONE,0 },{ SH_RECT,0,0,LWUI_COLOR_NONE,0 },
            { SH_POLY,0,0,LWUI_COLOR_NONE,0 },{ SH_OVAL,0,0,LWUI_COLOR_NONE,0 },{ SH_RROUND,0,0,LWUI_COLOR_NONE,0 } };
        LeewayRibbonSetCollection(g_ribbon, CMD_SHAPES, shapes, 6, 0);
        LeewayRibbonSetGalleryDrawCallback(g_ribbon, OnGalleryDraw, NULL);
        if (g_themePref >= 0) LeewayRibbonSetThemePreference(g_ribbon, (LeewayThemePref)g_themePref);
        LeewayRibbonAddRecentFile(g_ribbon, L"C:\\Docs\\Quarterly Report.rtf");
        LeewayRibbonAddRecentFile(g_ribbon, L"C:\\Docs\\Notes.txt");
        LeewayRibbonAddRecentFile(g_ribbon, L"C:\\Users\\me\\Letter.rtf");
        LeewayRibbonAddRecentFile(g_ribbon, L"D:\\Projects\\readme.md");
        return 0;
    }
    case WM_SIZE: {
        if (g_ribbon) {
            UINT h = LeewayRibbonGetHeight(g_ribbon);
            SetWindowPos(LeewayRibbonGetHWND(g_ribbon), 0, 0, 0, LOWORD(lp), h, SWP_NOZORDER);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == (UINT)LWUI_RBN_HEIGHTCHANGED) {
            RECT rc; GetClientRect(hwnd, &rc);
            SetWindowPos(LeewayRibbonGetHWND(g_ribbon), 0, 0, 0, rc.right, LeewayRibbonGetHeight(g_ribbon), SWP_NOZORDER);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        rc.top = LeewayRibbonGetHeight(g_ribbon);
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 12, rc.top + 10, g_status, (int)wcslen(g_status));
        /* a swatch showing the current text colour, proof the pick flowed back */
        RECT sw = { 12, rc.top + 34, 60, rc.top + 70 };
        HBRUSH b = CreateSolidBrush(g_textColor); FillRect(dc, &sw, b); DeleteObject(b);
        FrameRect(dc, &sw, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        if (g_ribbon) LeewayRibbonSetDPI(g_ribbon, HIWORD(wp));
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int show)
{
    /* args: --shot <file.bmp> [--surface appmenu|textcolor] */
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--shot") && i + 1 < argc) g_shot = argv[++i];
        else if (!wcscmp(argv[i], L"--surface") && i + 1 < argc) g_openSurface = argv[++i];
        else if (!wcscmp(argv[i], L"--width") && i + 1 < argc) g_width = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--theme") && i + 1 < argc) {
            const WCHAR *t = argv[++i];
            if (!wcscmp(t, L"light")) g_themePref = LEEWAY_THEME_PREF_LIGHT;
            else if (!wcscmp(t, L"dark")) g_themePref = LEEWAY_THEME_PREF_DARK;
            else g_themePref = LEEWAY_THEME_PREF_SYSTEM;
        }
    }
    (void)cmd;

    SetProcessDPIAware();
    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RibbonDemoMain";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"LeewayUI ribbon-f2 demo (WordPad shape)",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, g_width, 640,
                              0, 0, hi, 0);
    ShowWindow(hwnd, g_shot ? SW_SHOW : show);
    UpdateWindow(hwnd);

    if (g_shot) {
        auto pump = [](){ MSG m; while (PeekMessage(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } };
        for (int i = 0; i < 6; i++) { pump(); Sleep(120); }
        if (g_openSurface && !wcscmp(g_openSurface, L"overflow") && g_ribbon) {
            HWND rb = LeewayRibbonGetHWND(g_ribbon);
            RECT rc; GetClientRect(rb, &rc);
            HWND pop = NULL;
            for (int dx = 8; dx <= 30 && !pop; dx += 6) {
                int cx = rc.right - dx;
                for (int cy = 52; cy <= 150 && !pop; cy += 6) {
                    LPARAM pt = MAKELPARAM(cx, cy);
                    SendMessage(rb, WM_LBUTTONDOWN, MK_LBUTTON, pt);
                    SendMessage(rb, WM_LBUTTONUP, 0, pt);
                    pump();
                    pop = FindWindowW(L"LeewayUIRibbonPopup", NULL);
                }
            }
            for (int i = 0; i < 4; i++) { pump(); Sleep(60); }
            if (!pop) pop = FindWindowW(L"LeewayUIRibbonPopup", NULL);
            if (pop) { SaveHwndBmp(pop, g_shot); }
            else { fputws(L"NO POPUP", stderr); SaveWindowBmp(hwnd, g_shot); }
        } else if (g_openSurface && !wcscmp(g_openSurface, L"appmenu") && g_ribbon) {
            HWND rb = LeewayRibbonGetHWND(g_ribbon);
            const int cand[][2] = { {26,37},{26,50},{26,25},{40,37},{20,40},{26,103} };
            HWND pop = NULL;
            for (int c = 0; c < 6 && !pop; c++) {
                LPARAM pt = MAKELPARAM(cand[c][0], cand[c][1]);
                SendMessage(rb, WM_LBUTTONDOWN, MK_LBUTTON, pt);
                SendMessage(rb, WM_LBUTTONUP, 0, pt);
                for (int i = 0; i < 4; i++) { pump(); Sleep(80); }
                pop = FindWindowW(L"LeewayUIRibbonPopup", NULL);
            }
            if (pop) { SaveHwndBmp(pop, g_shot); }
            else { fputws(L"NO POPUP", stderr); SaveWindowBmp(hwnd, g_shot); }
        } else {
            SaveWindowBmp(hwnd, g_shot);
        }
        return 0;
    }

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0) > 0) {
        if (g_ribbon && LeewayRibbonPreTranslateMessage(g_ribbon, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
