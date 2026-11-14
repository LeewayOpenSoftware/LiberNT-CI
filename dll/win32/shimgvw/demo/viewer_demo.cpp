/*
 * PROJECT:     LiberNT Photo Viewer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Win7 Photo Viewer V0 on LeewayUI: menu-based UI with LeewayUI frame
 *              for chrome (status bar, zoom slider) and GDI+ image rendering.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: photoviewer.exe [image]   Ctrl+O opens, arrows page, Ctrl+wheel zooms.
 * V0: menu strip (File, Print, Open, Help), status bar with zoom slider.
 */

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <vector>
#include <leewayui/frame.h>

// Command IDs for menu
enum { ID_FILE_OPEN = 1001, ID_FILE_EXIT = 1002, ID_PRINT_PRINT = 2001, ID_HELP_ABOUT = 4001 };

static HINSTANCE g_inst;
static HWND g_hwnd;
static LeewayUIFrame *g_frame;
static RECT g_work;

static Gdiplus::Image *g_img;
static std::vector<std::wstring> g_files;
static size_t g_idx;
static UINT g_zoom = 100;
static bool g_fit = true;

/* ---- image ---- */

static std::wstring Base(const std::wstring &p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

static bool IsImageName(const std::wstring &n)
{
    static const wchar_t *ext[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".ico" };
    std::wstring l = n;
    for (auto &c : l)
        c = (wchar_t)towlower(c);
    for (auto e : ext)
    {
        size_t el = wcslen(e);
        if (l.size() >= el && !l.compare(l.size() - el, el, e))
            return true;
    }
    return false;
}

static void FitZoom()
{
    if (!g_img)
        return;
    double w = (double)g_img->GetWidth(), h = (double)g_img->GetHeight();
    double aw = g_work.right - g_work.left - 16, ah = g_work.bottom - g_work.top - 16;
    if (w < 1 || h < 1 || aw < 1 || ah < 1)
        return;
    double z = std::min(aw / w, ah / h) * 100.0;
    g_zoom = (UINT)std::max(1.0, std::min(z, 100.0));      /* fit never enlarges */
}

static void StatusInfo()
{
    wchar_t t[512] = L"";
    if (g_img && !g_files.empty())
        wsprintfW(t, L"%s   %u x %u   (%u of %u)", Base(g_files[g_idx]).c_str(), g_img->GetWidth(),
                  g_img->GetHeight(), (UINT)g_idx + 1, (UINT)g_files.size());
    LeewayFrameSetStatusText(g_frame, 0, t);
}

static void Refresh()
{
    if (g_fit)
        FitZoom();
    LeewayFrameSetZoom(g_frame, g_zoom);
    StatusInfo();
    InvalidateRect(g_hwnd, &g_work, FALSE);
}

static void LoadAt(size_t i)
{
    Gdiplus::Image *n = Gdiplus::Image::FromFile(g_files[i].c_str());
    if (!n || n->GetLastStatus() != Gdiplus::Ok)
    {
        delete n;
        MessageBoxW(g_hwnd, L"Cannot open this image.", L"Windows Photo Viewer", MB_ICONWARNING);
        return;
    }
    delete g_img;
    g_img = n;
    g_idx = i;
    g_fit = true;
    SetWindowTextW(g_hwnd, (Base(g_files[i]) + L" - Windows Photo Viewer").c_str());
    Refresh();
}

static void OpenFile(const std::wstring &path)
{
    std::wstring dir = path;
    size_t s = dir.find_last_of(L"\\/");
    dir = s == std::wstring::npos ? L"." : dir.substr(0, s);
    g_files.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageName(fd.cFileName))
                g_files.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_files.begin(), g_files.end(), [](const std::wstring &a, const std::wstring &b)
              { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i < g_files.size(); ++i)
        if (!lstrcmpiW(g_files[i].c_str(), path.c_str()))
        {
            at = i;
            found = true;
        }
    if (!found)
    {
        g_files.push_back(path);
        at = g_files.size() - 1;
    }
    LoadAt(at);
}

static void OpenDialog()
{
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH;
    o.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico\0All files\0*.*\0";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o))
        OpenFile(buf);
}

static void SetZoomPercent(UINT p)
{
    g_fit = false;
    g_zoom = std::max(1u, std::min(p, 1600u));
    Refresh();
}

static void StepZoom(int dir)
{
    UINT cur = g_zoom;
    if (dir > 0)
    {
        for (UINT i = 0; i < VRB_ZOOM_LEVELS; ++i)
            if (g_VrbZoomPercents[i] > cur)
            {
                SetZoomPercent(g_VrbZoomPercents[i]);
                return;
            }
    }
    else
    {
        for (int i = VRB_ZOOM_LEVELS - 1; i >= 0; --i)
            if (g_VrbZoomPercents[i] < cur)
            {
                SetZoomPercent(g_VrbZoomPercents[i]);
                return;
            }
    }
}

static void Rotate(Gdiplus::RotateFlipType t)
{
    if (!g_img)
        return;
    g_img->RotateFlip(t);
    Refresh();
}

static void Page(int d)
{
    if (g_files.size() < 2)
        return;
    LoadAt((g_idx + g_files.size() + d) % g_files.size());
}

/* ---- frame callbacks ---- */

static void OnFrameZoom(UINT32 p, void *) { SetZoomPercent(p); }
static void Layout();
static void OnFrameRelayout(const LeewayUIFrameRects *, void *) { Layout(); }

/* ---- menu creation ---- */

static HMENU CreateMenuBar()
{
    HMENU hMenu = CreateMenuW();
    HMENU hFileMenu = CreateMenuW();
    AppendMenuW(hFileMenu, MFT_STRING, ID_FILE_OPEN, L"&Open\tCtrl+O");
    AppendMenuW(hFileMenu, MFT_SEPARATOR, 0, nullptr);
    AppendMenuW(hFileMenu, MFT_STRING, ID_FILE_EXIT, L"E&xit");
    AppendMenuW(hMenu, MFT_POPUP, (UINT_PTR)hFileMenu, L"&File");

    HMENU hPrintMenu = CreateMenuW();
    AppendMenuW(hPrintMenu, MFT_STRING, ID_PRINT_PRINT, L"&Print");
    AppendMenuW(hMenu, MFT_POPUP, (UINT_PTR)hPrintMenu, L"&Print");

    HMENU hHelpMenu = CreateMenuW();
    AppendMenuW(hHelpMenu, MFT_STRING, ID_HELP_ABOUT, L"&About");
    AppendMenuW(hMenu, MFT_POPUP, (UINT_PTR)hHelpMenu, L"&Help");

    return hMenu;
}

/* ---- window ---- */

static void Layout()
{
    RECT rc;
    LeewayUIFrameRects fr;
    GetClientRect(g_hwnd, &rc);
    LeewayFrameLayout(g_frame, &rc, &fr);
    g_work = fr.work;
    if (g_fit)
        FitZoom(), LeewayFrameSetZoom(g_frame, g_zoom);
    InvalidateRect(g_hwnd, &g_work, FALSE);
}

static void Paint(HDC dc)
{
    int w = g_work.right - g_work.left, h = g_work.bottom - g_work.top;
    if (w <= 0 || h <= 0)
        return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r = { 0, 0, w, h };
    HBRUSH br = CreateSolidBrush(RGB(32, 32, 34));
    FillRect(mem, &r, br);
    DeleteObject(br);
    if (g_img)
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(g_zoom < 100 ? Gdiplus::InterpolationModeHighQualityBicubic
                                            : Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        int iw = (int)(g_img->GetWidth() * g_zoom / 100.0), ih = (int)(g_img->GetHeight() * g_zoom / 100.0);
        g.DrawImage(g_img, (w - iw) / 2, (h - ih) / 2, std::max(iw, 1), std::max(ih, 1));
    }
    else
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(190, 190, 195));
        DrawTextW(mem, L"Ctrl+O to open a picture (or drop one here)", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        case WM_COMMAND:
        {
            UINT id = LOWORD(wp);
            switch (id)
            {
                case ID_FILE_OPEN:  OpenDialog(); break;
                case ID_FILE_EXIT:  DestroyWindow(hwnd); break;
                case ID_PRINT_PRINT: MessageBoxW(hwnd, L"Print not implemented in V0.", L"Photo Viewer", MB_ICONINFORMATION); break;
                case ID_HELP_ABOUT: MessageBoxW(hwnd, L"Windows Photo Viewer V0 on LeewayUI\nImage navigation: arrows, Ctrl+wheel to zoom, Ctrl+O to open",
                                                L"Windows Photo Viewer", MB_OK | MB_ICONINFORMATION); break;
            }
            return 0;
        }
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
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
                StepZoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            else
                Page(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_LEFT) Page(-1);
            else if (wp == VK_RIGHT) Page(1);
            else if (wp == VK_ADD || wp == VK_OEM_PLUS) StepZoom(1);
            else if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS) StepZoom(-1);
            else if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) OpenDialog();
            return 0;
        case WM_DROPFILES:
        {
            wchar_t f[MAX_PATH];
            if (DragQueryFileW((HDROP)wp, 0, f, MAX_PATH))
                OpenFile(f);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_DESTROY:
            LeewayFrameDestroy(g_frame);
            g_frame = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PhotoViewerW7";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Windows Photo Viewer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, NULL, NULL, inst, NULL);

    // Create menu bar
    SetMenu(g_hwnd, CreateMenuBar());

    // Create LeewayUI frame for status bar and zoom slider (no ribbon)
    static const UINT32 kZoomLevels[] = { 10, 25, 50, 75, 100, 150, 200, 400, 800, 1600 };
    LeewayUIFrameDesc d = {};
    d.struct_size = sizeof(d);
    d.owner = g_hwnd;
    d.instance = inst;
    d.app_name = L"PhotoViewer";
    d.flags = LWUI_FRAME_STATUSBAR | LWUI_FRAME_ZOOM_SLIDER;
    d.default_chrome = LWUI_CHROME_PILL;
    d.pill_height = 40;
    d.zoom_levels = kZoomLevels;
    d.zoom_level_count = 10;
    d.on_zoom = OnFrameZoom;
    d.on_layout = OnFrameRelayout;
    if (FAILED(LeewayFrameCreate(&d, &g_frame)))
    {
        MessageBoxW(NULL, L"Could not create the frame.", L"Windows Photo Viewer", MB_ICONERROR);
        return 1;
    }
    Layout();
    Refresh();
    ShowWindow(g_hwnd, show);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
        OpenFile(argv[1]);
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Gdiplus::GdiplusShutdown(tok);
    return 0;
}
