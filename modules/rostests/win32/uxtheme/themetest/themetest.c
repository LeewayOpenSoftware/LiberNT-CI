/*
 * PROJECT:     LiberNT tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     themetest: one window, every part and state of the common controls drawn through the public uxtheme API
 *              (DrawThemeBackground, DrawThemeText, GetThemePartSize), a label per cell, a screenshot to a BMP file and
 *              a CSV of the part sizes, for the side-by-side comparison LiberNT vs Windows (owner order 2026-11-15);
 *              a Style menu and a batch mode switch between the visual styles installed in the store (owner demo 2026-11-16)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: themetest [--class NAME] [--part N] [--wave N] [--out FILE.bmp] [--csv FILE.csv] [--wait MS] [--scale N]
 *                  [--list] [--style FILE.msstyles] [--all-styles DIR]
 *   --class  only the rows of that theme class (Button, Edit, Combobox, Scrollbar, Progress, Tab, Header, Window,
 *            Toolbar, Rebar, Status, Menu, Spin, Trackbar, Tooltip, ListView, TreeView)
 *   --part   only that part id of the class
 *   --wave   only one group of rows: 1 = the whole first table (Button .. Window, 1166 px: taller than a 1280x800
 *            screen, the capture is cut at the screen), 2 = Toolbar .. Spin, 3 = Trackbar .. TreeView, and the first
 *            table in screen-sized pieces: 4 = Button .. Combobox, 5 = Scrollbar .. Header, 6 = Window;
 *            7 = the Windows 10 taskbar classes TaskBar, TaskBand, TrayNotify; 8 = StartPanel
 *   --out    write a screenshot of the client area after the first paint, then exit (default: stay open)
 *   --csv    write "class,part,state,TS_MIN w,h,TS_TRUE w,h,TS_DRAW w,h,hr" per cell
 *   --wait   milliseconds to wait before the screenshot (default 500)
 *   --scale  cell scale factor 1..4 (default 1)
 *   --list   print the visual styles of the store (%windir%\Resources\Themes\<name>\*.msstyles) and exit
 *   --style  apply that style file first (SetSystemVisualStyle, the same call the Personalization page makes)
 *   --all-styles  for every style of the store (DIR is created first, exit code 6 when that fails): apply it, repaint,
 *                 write DIR\<name>.bmp and DIR\<name>.csv, then put the
 *                 (a style that stalls for longer than 15 s + 4 x wait ends the run with DIR\<name>.stall.txt naming the
 *                 stage and the cell, exit code 7)
 *            style that was active back and exit
 * The window has a Style menu with the same list. The tool reads no file itself; the style switch is the public
 * uxtheme call and the system decides whether the style is trusted (a refused style leaves the current one active, which
 * the title bar and the stdout line show). The same source builds with mingw (gcc -municode themetest.c -luxtheme
 * -lgdi32 -luser32) for the Windows reference capture.
 */
#include <windows.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct row {
    const wchar_t *cls;      /* theme class for OpenThemeData */
    int part;
    const wchar_t *name;     /* part name for the label */
    int first_state, last_state;
    int cw, ch;              /* cell size in pixels at scale 1 */
    int text;                /* draw sample text in the cell */
};

/* The parts of the owner's list, states as vsstyle.h numbers them. */
static const struct row ROWS[] = {
    { L"Button", BP_PUSHBUTTON, L"PushButton", PBS_NORMAL, PBS_DEFAULTED_ANIMATING, 96, 28, 1 },
    { L"Button", BP_RADIOBUTTON, L"RadioButton", RBS_UNCHECKEDNORMAL, RBS_CHECKEDDISABLED, 24, 24, 0 },
    { L"Button", BP_CHECKBOX, L"CheckBox", CBS_UNCHECKEDNORMAL, CBS_MIXEDDISABLED, 24, 24, 0 },
    { L"Button", BP_GROUPBOX, L"GroupBox", GBS_NORMAL, GBS_DISABLED, 96, 48, 0 },
    { L"Edit", EP_EDITTEXT, L"EditText", ETS_NORMAL, ETS_ASSIST, 96, 24, 1 },
    { L"Edit", EP_EDITBORDER_NOSCROLL, L"EditBorder", EPSN_NORMAL, EPSN_DISABLED, 96, 24, 0 },
    { L"Combobox", CP_DROPDOWNBUTTON, L"DropDownBtn", CBXS_NORMAL, CBXS_DISABLED, 24, 24, 0 },
    { L"Combobox", CP_BORDER, L"ComboBorder", CBB_NORMAL, CBB_DISABLED, 96, 24, 0 },
    { L"Combobox", CP_READONLY, L"ReadOnly", CBRO_NORMAL, CBRO_DISABLED, 96, 24, 1 },
    { L"Scrollbar", SBP_ARROWBTN, L"ArrowBtn", ABS_UPNORMAL, ABS_RIGHTHOVER, 20, 20, 0 },
    { L"Scrollbar", SBP_THUMBBTNHORZ, L"ThumbHorz", SCRBS_NORMAL, SCRBS_HOVER, 48, 20, 0 },
    { L"Scrollbar", SBP_THUMBBTNVERT, L"ThumbVert", SCRBS_NORMAL, SCRBS_HOVER, 20, 48, 0 },
    { L"Scrollbar", SBP_LOWERTRACKHORZ, L"TrackHorz", SCRBS_NORMAL, SCRBS_HOVER, 48, 20, 0 },
    { L"Scrollbar", SBP_GRIPPERHORZ, L"GripHorz", SCRBS_NORMAL, SCRBS_HOVER, 48, 20, 0 },
    { L"Progress", PP_BAR, L"Bar", 1, 1, 120, 20, 0 },
    { L"Progress", PP_CHUNK, L"Chunk", 1, 1, 60, 16, 0 },
    { L"Progress", PP_FILL, L"Fill", PBFS_NORMAL, PBFS_PARTIAL, 60, 16, 0 },
    { L"Tab", TABP_TABITEM, L"TabItem", TIS_NORMAL, TIS_FOCUSED, 72, 24, 1 },
    { L"Tab", TABP_TOPTABITEM, L"TopTabItem", TIS_NORMAL, TIS_FOCUSED, 72, 24, 1 },
    { L"Tab", TABP_PANE, L"Pane", 1, 1, 96, 48, 0 },
    { L"Header", HP_HEADERITEM, L"HeaderItem", HIS_NORMAL, HIS_PRESSED, 96, 24, 1 },
    { L"Window", WP_CAPTION, L"Caption", CS_ACTIVE, CS_DISABLED, 160, 30, 1 },
    { L"Window", WP_CLOSEBUTTON, L"CloseBtn", CBS_NORMAL, CBS_DISABLED, 46, 20, 0 },
    { L"Window", WP_MINBUTTON, L"MinBtn", MINBS_NORMAL, MINBS_DISABLED, 28, 20, 0 },
    { L"Window", WP_MAXBUTTON, L"MaxBtn", MAXBS_NORMAL, MAXBS_DISABLED, 28, 20, 0 },
    { L"Window", WP_FRAMELEFT, L"FrameLeft", FS_ACTIVE, FS_INACTIVE, 12, 48, 0 },
    { L"Window", WP_FRAMEBOTTOM, L"FrameBottom", FS_ACTIVE, FS_INACTIVE, 96, 12, 0 },
    /* second wave (owner, 2026-11-16): the rest of the common controls */
    { L"Toolbar", TP_BUTTON, L"Button", TS_NORMAL, TS_HOTCHECKED, 48, 24, 0 },
    { L"Toolbar", TP_SEPARATOR, L"Separator", TS_NORMAL, TS_NORMAL, 12, 24, 0 },
    { L"Rebar", RP_BAND, L"Band", 1, 1, 96, 24, 0 },
    { L"Rebar", RP_GRIPPER, L"Gripper", 1, 1, 12, 24, 0 },
    { L"Rebar", RP_CHEVRON, L"Chevron", CHEVS_NORMAL, CHEVS_PRESSED, 24, 24, 0 },
    { L"Status", SP_PANE, L"Pane", 1, 1, 96, 22, 1 },
    { L"Status", SP_GRIPPER, L"Gripper", 1, 1, 20, 20, 0 },
    { L"Menu", MENU_BARBACKGROUND, L"BarBackground", MB_ACTIVE, MB_INACTIVE, 96, 22, 0 },
    { L"Menu", MENU_BARITEM, L"BarItem", MBI_NORMAL, MBI_DISABLEDPUSHED, 48, 22, 1 },
    { L"Menu", MENU_POPUPITEM, L"PopupItem", MPI_NORMAL, MPI_DISABLEDHOT, 96, 22, 1 },
    { L"Menu", MENU_POPUPCHECK, L"PopupCheck", MC_CHECKMARKNORMAL, MC_BULLETDISABLED, 24, 22, 0 },
    { L"Menu", MENU_POPUPSEPARATOR, L"PopupSeparator", 1, 1, 96, 8, 0 },
    { L"Spin", SPNP_UP, L"Up", UPS_NORMAL, UPS_DISABLED, 20, 14, 0 },
    { L"Spin", SPNP_DOWN, L"Down", DNS_NORMAL, DNS_DISABLED, 20, 14, 0 },
    { L"Trackbar", TKP_TRACK, L"Track", TKS_NORMAL, TKS_NORMAL, 96, 8, 0 },
    { L"Trackbar", TKP_THUMB, L"Thumb", TUS_NORMAL, TUS_DISABLED, 16, 24, 0 },
    { L"Trackbar", TKP_THUMBBOTTOM, L"ThumbBottom", TUBS_NORMAL, TUBS_DISABLED, 16, 24, 0 },
    { L"Tooltip", TTP_STANDARD, L"Standard", TTSS_NORMAL, TTSS_LINK, 96, 24, 1 },
    { L"ListView", LVP_LISTITEM, L"ListItem", LISS_NORMAL, LISS_HOTSELECTED, 96, 22, 1 },
    { L"TreeView", TVP_TREEITEM, L"TreeItem", TREIS_NORMAL, TREIS_HOTSELECTED, 96, 22, 1 },
    { L"TreeView", TVP_GLYPH, L"Glyph", GLPS_CLOSED, GLPS_OPENED, 16, 16, 0 },
    /* waves 7 and 8 (owner, 2026-11-16): the classes of the Windows 10 taskbar, drawn from the style like any other part */
    { L"TaskBar", TBP_BACKGROUNDBOTTOM, L"BackgroundBottom", 1, 1, 160, 30, 0 },
    { L"TaskBar", TBP_BACKGROUNDTOP, L"BackgroundTop", 1, 1, 160, 30, 0 },
    { L"TaskBar", TBP_BACKGROUNDLEFT, L"BackgroundLeft", 1, 1, 30, 60, 0 },
    { L"TaskBar", TBP_SIZINGBARBOTTOM, L"SizingBarBottom", 1, 1, 160, 8, 0 },
    { L"TaskBand", TDP_GROUPCOUNT, L"GroupCount", 1, 1, 24, 24, 0 },
    { L"TaskBand", TDP_FLASHBUTTON, L"FlashButton", 1, 1, 96, 30, 1 },
    { L"TaskBand", TDP_FLASHBUTTONGROUPMENU, L"FlashGroupMenu", 1, 1, 96, 30, 1 },
    { L"TrayNotify", TNP_BACKGROUND, L"Background", 1, 1, 96, 30, 0 },
    { L"TrayNotify", TNP_ANIMBACKGROUND, L"AnimBackground", 1, 1, 96, 30, 0 },
    { L"StartPanel", SPP_USERPANE, L"UserPane", 1, 1, 96, 30, 0 },
    { L"StartPanel", SPP_MOREPROGRAMS, L"MorePrograms", 1, 1, 96, 30, 1 },
    { L"StartPanel", SPP_MOREPROGRAMSARROW, L"MoreProgramsArrow", 1, 3, 24, 24, 0 },
    { L"StartPanel", SPP_PROGLIST, L"ProgList", 1, 1, 96, 30, 0 },
    { L"StartPanel", SPP_PLACESLIST, L"PlacesList", 1, 1, 96, 30, 0 },
    { L"StartPanel", SPP_LOGOFF, L"Logoff", 1, 1, 96, 30, 0 },
    { L"StartPanel", SPP_LOGOFFBUTTONS, L"LogoffButtons", 1, 3, 48, 24, 0 },
    { L"StartPanel", SPP_USERPICTURE, L"UserPicture", 1, 1, 48, 48, 0 },
};
#define NROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static const wchar_t *g_class;
static int g_part = -1, g_scale = 1, g_wait = 500, g_wave;
static const wchar_t *g_out, *g_csv;
static int g_props;   /* --props: add the public style properties behind the size columns */
static int g_painted;
static int g_hr_count[2];           /* DrawThemeBackground succeeded / failed cells */

/* the visual styles of the store, one per directory of %windir%\Resources\Themes */
#define MAX_STYLES 64
#define IDM_STYLE_FIRST 1000
struct style { wchar_t name[64]; wchar_t path[MAX_PATH]; };
static struct style g_styles[MAX_STYLES];
static int g_nstyles;
static const wchar_t *g_batch_dir;  /* --all-styles */
static int g_batch_index = -1;      /* style being shot in batch mode */
static wchar_t g_batch_out[MAX_PATH], g_batch_csv[MAX_PATH];
static wchar_t g_initial_style[MAX_PATH];

/* batch watchdog: the stage and the cell being drawn, so a stall names them instead of hanging the leg */
static volatile LONG g_stage;                    /* 0 idle, 1 apply, 2 paint, 3 wait for the shot, 4 shot */
static volatile LONG g_cell_row = -1, g_cell_state;
static volatile DWORD g_stage_tick, g_style_tick;

static void mark(LONG stage)
{
    g_stage = stage;
    g_stage_tick = GetTickCount();
}

#define LABEL_H 16
#define GAP 8
#define MARGIN 12

/* the wave of a row: 2 from the first Toolbar row, 3 from the first Trackbar row, 1 before */
static int row_wave(const struct row *r)
{
    const struct row *p;
    int wave = 1;
    for (p = ROWS; p <= r; p++)
    {
        if (!wcscmp(p->cls, L"Toolbar") && wave < 2) wave = 2;
        if (!wcscmp(p->cls, L"Trackbar") && wave < 3) wave = 3;
        if (!wcscmp(p->cls, L"TaskBar") && wave < 7) wave = 7;
        if (!wcscmp(p->cls, L"StartPanel") && wave < 8) wave = 8;
    }
    return wave;
}

/* the first table in screen-sized pieces: 4 Button..Combobox, 5 Scrollbar..Header, 6 Window; 0 for the other tables */
static int row_subwave(const struct row *r)
{
    if (row_wave(r) != 1) return 0;
    if (!wcscmp(r->cls, L"Window")) return 6;
    if (!wcscmp(r->cls, L"Scrollbar") || !wcscmp(r->cls, L"Progress") || !wcscmp(r->cls, L"Tab") || !wcscmp(r->cls, L"Header")) return 5;
    return 4;
}

static int row_selected(const struct row *r)
{
    if (g_wave && row_wave(r) != g_wave && row_subwave(r) != g_wave) return 0;
    if (g_class && _wcsicmp(g_class, r->cls) != 0) return 0;
    if (g_part >= 0 && r->part != g_part) return 0;
    return 1;
}

/* layout: one row per table row, cells left to right; returns the client size needed */
static SIZE layout_size(void)
{
    SIZE s = { 0, MARGIN };
    int i;
    for (i = 0; i < NROWS; i++)
    {
        const struct row *r = &ROWS[i];
        int n = r->last_state - r->first_state + 1, w;
        if (!row_selected(r)) continue;
        w = MARGIN + 110 + n * (r->cw * g_scale + GAP) + MARGIN;
        if (w > s.cx) s.cx = w;
        s.cy += r->ch * g_scale + LABEL_H + GAP;
    }
    s.cy += MARGIN;
    if (s.cx < 200) s.cx = 200;
    if (s.cy < 60) s.cy = 60;
    return s;
}

static void csv_sizes(FILE *f, HTHEME h, HDC hdc, const struct row *r, int state)
{
    static const THEMESIZE ts[3] = { TS_MIN, TS_TRUE, TS_DRAW };
    int k;
    fwprintf(f, L"%ls,%d,%d", r->cls, r->part, state);
    for (k = 0; k < 3; k++)
    {
        SIZE sz = { -1, -1 };
        RECT rc = { 0, 0, r->cw * g_scale, r->ch * g_scale };
        HRESULT hr = h ? GetThemePartSize(h, hdc, r->part, state, &rc, ts[k], &sz) : E_HANDLE;
        if (SUCCEEDED(hr)) fwprintf(f, L",%ld,%ld", sz.cx, sz.cy); else fwprintf(f, L",err%08lx,", (unsigned long)hr);
    }
    if (g_props && h)
    {
        int v = 0; MARGINS m; POINT pt; HRESULT hr;
        hr = GetThemeEnumValue(h, r->part, state, TMT_BGTYPE, &v); fwprintf(f, SUCCEEDED(hr) ? L",%d" : L",", v);
        hr = GetThemeEnumValue(h, r->part, state, TMT_SIZINGTYPE, &v); fwprintf(f, SUCCEEDED(hr) ? L",%d" : L",", v);
        hr = GetThemeInt(h, r->part, state, TMT_IMAGECOUNT, &v); fwprintf(f, SUCCEEDED(hr) ? L",%d" : L",", v);
        hr = GetThemeEnumValue(h, r->part, state, TMT_IMAGELAYOUT, &v); fwprintf(f, SUCCEEDED(hr) ? L",%d" : L",", v);
        hr = GetThemeMargins(h, hdc, r->part, state, TMT_SIZINGMARGINS, NULL, &m);
        if (SUCCEEDED(hr)) fwprintf(f, L",%d,%d,%d,%d", m.cxLeftWidth, m.cxRightWidth, m.cyTopHeight, m.cyBottomHeight); else fwprintf(f, L",,,,");
        hr = GetThemePosition(h, r->part, state, TMT_MINSIZE, &pt);
        if (SUCCEEDED(hr)) fwprintf(f, L",%ld,%ld", pt.x, pt.y); else fwprintf(f, L",,");
    }
    fwprintf(f, L"\n");
    fflush(f);
}

static void paint(HWND hwnd, HDC hdc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT), old = (HFONT)SelectObject(hdc, font);
    int i, y = MARGIN;
    FILE *csv = 0;
    RECT client;

    GetClientRect(hwnd, &client);
    FillRect(hdc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(hdc, TRANSPARENT);
    if (g_csv && !g_painted) { csv = _wfopen(g_csv, L"w"); if (csv) fwprintf(csv, L"class,part,state,min_w,min_h,true_w,true_h,draw_w,draw_h%ls\n", g_props ? L",bgtype,sizingtype,imagecount,imagelayout,sm_l,sm_r,sm_t,sm_b,minsize_x,minsize_y" : L""); }
    g_hr_count[0] = g_hr_count[1] = 0;

    for (i = 0; i < NROWS; i++)
    {
        const struct row *r = &ROWS[i];
        HTHEME h;
        int state, x = MARGIN + 110;
        RECT lab = { MARGIN, y, MARGIN + 108, y + r->ch * g_scale };
        wchar_t text[64];
        if (!row_selected(r)) continue;
        h = OpenThemeData(hwnd, r->cls);
        swprintf(text, 64, L"%ls %ls (%d)%ls", r->cls, r->name, r->part, h ? L"" : L" NO THEME");
        SetTextColor(hdc, h ? RGB(0, 0, 0) : RGB(200, 0, 0));
        DrawTextW(hdc, text, -1, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        for (state = r->first_state; state <= r->last_state; state++)
        {
            RECT rc = { x, y, x + r->cw * g_scale, y + r->ch * g_scale };
            RECT lr = { x, y + r->ch * g_scale, x + r->cw * g_scale + GAP, y + r->ch * g_scale + LABEL_H };
            HRESULT hr = E_HANDLE;
            if (h)
            {
                g_cell_row = i;
                g_cell_state = state;
                hr = DrawThemeBackground(h, hdc, r->part, state, &rc, NULL);
                if (SUCCEEDED(hr) && r->text)
                    DrawThemeText(h, hdc, r->part, state, L"Sample", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 0, &rc);
                if (csv) csv_sizes(csv, h, hdc, r, state);
            }
            if (FAILED(hr))
            {
                HBRUSH hb = CreateSolidBrush(RGB(255, 200, 200));
                FrameRect(hdc, &rc, hb);
                DeleteObject(hb);
                g_hr_count[1]++;
            }
            else
                g_hr_count[0]++;
            swprintf(text, 64, L"%d%ls", state, FAILED(hr) ? L" x" : L"");
            SetTextColor(hdc, FAILED(hr) ? RGB(200, 0, 0) : RGB(80, 80, 80));
            DrawTextW(hdc, text, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE);
            x += r->cw * g_scale + GAP;
        }
        if (h) CloseThemeData(h);
        y += r->ch * g_scale + LABEL_H + GAP;
    }
    if (csv) fclose(csv);
    g_cell_row = -1;
    SelectObject(hdc, old);
}

/* the client area into a 24-bit BMP (no GDI+, no codec: the same bytes on every system) */
static int screenshot(HWND hwnd, const wchar_t *path)
{
    RECT rc;
    HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
    int w, h, rc_ok = 0;
    HBITMAP bmp, old;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER bf;
    DWORD stride, size;
    void *bits;
    FILE *f;

    GetClientRect(hwnd, &rc);
    w = rc.right; h = rc.bottom;
    bmp = CreateCompatibleBitmap(wdc, w, h);
    old = (HBITMAP)SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);

    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    stride = ((DWORD)w * 3 + 3) & ~3u;
    size = stride * (DWORD)h;
    bits = malloc(size);
    if (bits && GetDIBits(mdc, bmp, 0, (UINT)h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS) == h)
    {
        f = _wfopen(path, L"wb");
        if (f)
        {
            memset(&bf, 0, sizeof(bf));
            bf.bfType = 0x4D42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + size;
            fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f); fwrite(bits, 1, size, f);
            fclose(f);
            rc_ok = 1;
        }
    }
    free(bits);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return rc_ok;
}

/* ---- visual styles of the store ---- */

/* %windir%\Resources\Themes\<name>\*.msstyles, the first style file of each directory, in directory order */
static void enum_styles(void)
{
    wchar_t root[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW d, f;
    HANDLE hd, hf;

    g_nstyles = 0;
    if (!GetWindowsDirectoryW(root, MAX_PATH)) return;
    if (swprintf(pattern, MAX_PATH, L"%ls\\Resources\\Themes\\*", root) < 0) return;
    hd = FindFirstFileW(pattern, &d);
    if (hd == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
        if (swprintf(pattern, MAX_PATH, L"%ls\\Resources\\Themes\\%ls\\*.msstyles", root, d.cFileName) < 0) continue;
        hf = FindFirstFileW(pattern, &f);
        if (hf == INVALID_HANDLE_VALUE) continue;
        if (g_nstyles < MAX_STYLES &&
            swprintf(g_styles[g_nstyles].path, MAX_PATH, L"%ls\\Resources\\Themes\\%ls\\%ls", root, d.cFileName, f.cFileName) > 0)
        {
            wcsncpy(g_styles[g_nstyles].name, d.cFileName, 63);
            g_styles[g_nstyles].name[63] = 0;
            g_nstyles++;
        }
        FindClose(hf);
    } while (FindNextFileW(hd, &d));
    FindClose(hd);
}

/* the style file in use, "" when no style is active */
static void current_style(wchar_t *buf, int cch)
{
    buf[0] = 0;
    if (FAILED(GetCurrentThemeName(buf, cch, NULL, 0, NULL, 0))) buf[0] = 0;
}

/* SetSystemVisualStyle is exported by ordinal only (65), here and on Windows */
typedef HRESULT (WINAPI *PFN_SETSYSTEMVISUALSTYLE)(LPCWSTR pszFile, LPCWSTR pszColor, LPCWSTR pszSize, UINT uFlags);

static HRESULT apply_style(const wchar_t *path)
{
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    PFN_SETSYSTEMVISUALSTYLE fn = ux ? (PFN_SETSYSTEMVISUALSTYLE)GetProcAddress(ux, (LPCSTR)(ULONG_PTR)65) : NULL;
    HRESULT hr = fn ? fn(path, NULL, NULL, 0) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    wchar_t now[MAX_PATH];
    current_style(now, MAX_PATH);
    wprintf(L"themetest: style %ls -> hr=0x%08lx, active now: %ls\n", path, (unsigned long)hr, now[0] ? now : L"(none)");
    return hr;
}

static void set_title(HWND hwnd)
{
    wchar_t now[MAX_PATH], title[MAX_PATH + 64];
    current_style(now, MAX_PATH);
    swprintf(title, MAX_PATH + 64, L"themetest: %ls", now[0] ? now : L"no visual style active");
    SetWindowTextW(hwnd, title);
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), styles = CreatePopupMenu();
    int i;
    if (!bar || !styles) return NULL;
    for (i = 0; i < g_nstyles; i++)
        AppendMenuW(styles, MF_STRING, IDM_STYLE_FIRST + i, g_styles[i].name);
    if (!g_nstyles) AppendMenuW(styles, MF_STRING | MF_GRAYED, IDM_STYLE_FIRST, L"(no style in the store)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)styles, L"&Style");
    return bar;
}

/* batch: apply style i and let the next paint arm the screenshot timer; returns 0 when the list is done */
static int batch_start(HWND hwnd, int i)
{
    if (i >= g_nstyles) return 0;
    g_batch_index = i;
    g_style_tick = GetTickCount();
    mark(1);
    swprintf(g_batch_out, MAX_PATH, L"%ls\\%ls.bmp", g_batch_dir, g_styles[i].name);
    swprintf(g_batch_csv, MAX_PATH, L"%ls\\%ls.csv", g_batch_dir, g_styles[i].name);
    g_out = g_batch_out;
    g_csv = g_batch_csv;
    apply_style(g_styles[i].path);
    g_painted = 0;
    mark(2);
    InvalidateRect(hwnd, NULL, TRUE);
    return 1;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    for (;;)
    {
        DWORD now, limit = 15000 + 4 * (DWORD)g_wait;
        wchar_t line[256], path[MAX_PATH];
        FILE *f;
        LONG row, stage, idx;

        Sleep(500);
        /* one read of each shared value: the main thread moves them while this runs */
        idx = g_batch_index;
        stage = g_stage;
        row = g_cell_row;
        if (idx < 0 || idx >= g_nstyles || stage == 0) continue;
        now = GetTickCount();
        if (now - g_stage_tick <= limit && now - g_style_tick <= 60000 + 10 * (DWORD)g_wait) continue;
        swprintf(line, 256, L"themetest: STALL style %ls stage %ld cell %ls/%d/%ld after %lu ms (stage %lu ms)\n",
                 g_styles[idx].name, (long)stage, row >= 0 && row < NROWS ? ROWS[row].cls : L"-",
                 row >= 0 && row < NROWS ? ROWS[row].part : 0, (long)g_cell_state,
                 (unsigned long)(now - g_style_tick), (unsigned long)(now - g_stage_tick));
        wprintf(L"%ls", line);
        OutputDebugStringW(line);
        swprintf(path, MAX_PATH, L"%ls\\%ls.stall.txt", g_batch_dir, g_styles[idx].name);
        if ((f = _wfopen(path, L"w")) != NULL) { fputws(line, f); fclose(f); }
        TerminateProcess(GetCurrentProcess(), 7);
    }
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_CREATE:
            set_title(hwnd);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint(hwnd, hdc);
            EndPaint(hwnd, &ps);
            if (!g_painted)
            {
                g_painted = 1;
                if (g_out) SetTimer(hwnd, 1, (UINT)g_wait, NULL);
                mark(3);
            }
            return 0;
        }
        case WM_TIMER:
        {
            int ok;
            KillTimer(hwnd, 1);
            mark(4);
            ok = screenshot(hwnd, g_out);
            wprintf(L"themetest: %d cells drawn, %d failed; screenshot %ls: %ls\n", g_hr_count[0], g_hr_count[1], g_out, ok ? L"written" : L"FAILED");
            if (g_batch_index >= 0 && batch_start(hwnd, g_batch_index + 1))
                return 0;
            mark(0);
            if (g_batch_index >= 0 && g_initial_style[0])
                apply_style(g_initial_style);
            PostQuitMessage(ok ? 0 : 3);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) >= IDM_STYLE_FIRST && LOWORD(wp) < IDM_STYLE_FIRST + g_nstyles)
            {
                apply_style(g_styles[LOWORD(wp) - IDM_STYLE_FIRST].path);
                set_title(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            break;
        case WM_THEMECHANGED:
            /* a switch lands after our own repaint: paint again and, when a shot is pending, arm the timer again */
            set_title(hwnd);
            if (g_out) g_painted = 0;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t **argv)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG m;
    SIZE need;
    RECT wr;
    int i, list = 0;
    const wchar_t *style = NULL;

    setvbuf(stdout, NULL, _IONBF, 0);   /* a redirected run shows its progress up to a stop */
    for (i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--class") && i + 1 < argc) g_class = argv[++i];
        else if (!wcscmp(argv[i], L"--part") && i + 1 < argc) g_part = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--wave") && i + 1 < argc) g_wave = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) g_out = argv[++i];
        else if (!wcscmp(argv[i], L"--csv") && i + 1 < argc) g_csv = argv[++i];
        else if (!wcscmp(argv[i], L"--wait") && i + 1 < argc) g_wait = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--scale") && i + 1 < argc) { g_scale = _wtoi(argv[++i]); if (g_scale < 1) g_scale = 1; if (g_scale > 4) g_scale = 4; }
        else if (!wcscmp(argv[i], L"--props")) g_props = 1;
        else if (!wcscmp(argv[i], L"--list")) list = 1;
        else if (!wcscmp(argv[i], L"--style") && i + 1 < argc) style = argv[++i];
        else if (!wcscmp(argv[i], L"--all-styles") && i + 1 < argc) g_batch_dir = argv[++i];
        else
        {
            wprintf(L"usage: themetest [--class NAME] [--part N] [--wave N] [--out FILE.bmp] [--csv FILE.csv] [--wait MS] [--scale N]\n"
                    L"                 [--list] [--style FILE.msstyles] [--all-styles DIR]\n");
            return 2;
        }
    }
    wprintf(L"themetest: IsAppThemed %d, IsThemeActive %d\n", IsAppThemed(), IsThemeActive());
    current_style(g_initial_style, MAX_PATH);
    wprintf(L"themetest: active style: %ls\n", g_initial_style[0] ? g_initial_style : L"(none)");
    enum_styles();
    if (list)
    {
        for (i = 0; i < g_nstyles; i++) wprintf(L"%ls\t%ls\n", g_styles[i].name, g_styles[i].path);
        wprintf(L"themetest: %d style(s) in the store\n", g_nstyles);
        return 0;
    }
    /* before any style change: a failure here must leave the active style as it was */
    if (g_batch_dir && !CreateDirectoryW(g_batch_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        wprintf(L"themetest: cannot create %ls (error %lu)\n", g_batch_dir, GetLastError());
        return 6;
    }
    if (style && FAILED(apply_style(style))) return 4;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TL4ThemeTest";
    if (!RegisterClassW(&wc)) return 2;

    need = layout_size();
    SetRect(&wr, 0, 0, need.cx, need.cy);
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, TRUE);
    /* top left: a tall window placed by the system can end under the taskbar, which the screen copy then shows */
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"themetest", WS_OVERLAPPEDWINDOW,
                           0, 0, wr.right - wr.left, wr.bottom - wr.top, NULL, build_menu(), wc.hInstance, NULL);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    if (g_batch_dir)
    {
        HANDLE wd;
        if (!g_nstyles) { wprintf(L"themetest: no style in the store, nothing to shoot\n"); return 5; }
        if ((wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL)) != NULL) CloseHandle(wd);
        batch_start(hwnd, 0);
    }
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return (int)m.wParam;
}
