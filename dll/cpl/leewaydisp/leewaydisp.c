/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Control Panel item: Personalization (wallpaper) and Display (resolution) over the Leeway backend
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <commctrl.h>
#include <cpl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <strsafe.h>
#include <winreg.h>
#include <wchar.h>
#include <leewayui/display_backend.h>
#include "resource.h"
#include "internal.h"

#define KEEP_SECONDS 15
#define TIMER_KEEP   1
/* posted by the main dialog to itself: show and activate it before the modal loop does */
#define WM_LDISP_ACTIVATE (WM_APP + 1)

typedef struct APPLET
{
    UINT NameId;
    UINT DescId;
    int Tab;
} APPLET;

static const APPLET Applets[] =
{
    { IDS_CPL_PERSONALIZE_NAME, IDS_CPL_PERSONALIZE_DESC, 0 },
    { IDS_CPL_DISPLAY_NAME, IDS_CPL_DISPLAY_DESC, 1 },
};

HINSTANCE hInst;

static DWORD CapsValue;
static BOOL CapsValid;

void
CapsReset(void)
{
    CapsValid = FALSE;
}

/* One query per applet run; the backend probes the system on every call. */
DWORD
Caps(void)
{
    if (!CapsValid)
    {
        CapsValue = LDispCapabilities();
        CapsValid = TRUE;
    }
    return CapsValue;
}

typedef struct MAINDATA
{
    HWND Tab;
    HWND Pages[4];
    int PageCount;
    ULONG_PTR GdipToken;
    int InitialTab;
} MAINDATA;

typedef struct PERSDATA
{
    LWALL_ITEM Items[LWALL_MAX_ITEMS];
    int Count;
    int Selected;
} PERSDATA;

typedef struct DISPDATA
{
    LDISP_MONITOR Monitors[LDISP_MAX_MONITORS];
    int MonitorCount;
    LDISP_MODE Modes[LDISP_MAX_MODES];
    int ModeCount;
    LDISP_MODE Sizes[LDISP_MAX_MODES];  /* unique width x height */
    int SizeCount;
    DWORD Freqs[64];
    int FreqCount;
    LDISP_MODE Current;
    BOOL HaveCurrent;
    RECT MonRect[LDISP_MAX_MONITORS];  /* picture hit areas, filled when drawn */
} DISPDATA;

/* HELPERS ******************************************************************/

void
LoadStr(UINT id, WCHAR *buf, int cch)
{
    if (!LoadStringW(hInst, id, buf, cch))
        buf[0] = 0;
}

void
Msg(HWND hwnd, UINT textId, UINT flags)
{
    WCHAR text[256], title[64];

    LoadStr(textId, text, ARRAYSIZE(text));
    LoadStr(IDS_TITLE, title, ARRAYSIZE(title));
    MessageBoxW(hwnd, text, title, flags);
}

/* PERSONALIZATION PAGE *****************************************************/

static void
PaintPreview(HWND hwnd, DRAWITEMSTRUCT *dis, PERSDATA *pd)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    HBRUSH frame = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
    GpGraphics *graphics = NULL;
    GpBitmap *bitmap = NULL;

    FillRect(hdc, &rc, GetSysColorBrush(COLOR_DESKTOP));
    if (pd->Selected >= 0 && pd->Selected < pd->Count &&
        GdipCreateBitmapFromFile(pd->Items[pd->Selected].Path, &bitmap) == Ok)
    {
        UINT w = 0, h = 0;
        int cx = rc.right - rc.left, cy = rc.bottom - rc.top, dw, dh;

        GdipGetImageWidth((GpImage *)bitmap, &w);
        GdipGetImageHeight((GpImage *)bitmap, &h);
        if (w && h && GdipCreateFromHDC(hdc, &graphics) == Ok)
        {
            /* the preview always shows the picture fitted, whatever the chosen position */
            if ((__int64)w * cy > (__int64)h * cx)
            {
                dw = cx;
                dh = (int)((__int64)h * cx / w);
            }
            else
            {
                dh = cy;
                dw = (int)((__int64)w * cy / h);
            }
            GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBilinear);
            GdipDrawImageRectI(graphics, (GpImage *)bitmap, rc.left + (cx - dw) / 2, rc.top + (cy - dh) / 2, dw, dh);
            GdipDeleteGraphics(graphics);
        }
        GdipDisposeImage((GpImage *)bitmap);
    }
    FrameRect(hdc, &rc, frame);
    DeleteObject(frame);
    UNREFERENCED_PARAMETER(hwnd);
}

static void
UpdateCurrentLabel(HWND hwnd)
{
    WCHAR path[MAX_PATH] = L"", fmt[128], text[MAX_PATH + 160];
    LWALL_STYLE style;
    HWND combo = GetDlgItem(hwnd, IDC_WALL_STYLE);

    LoadStr(IDS_CURRENT_WALLPAPER, fmt, ARRAYSIZE(fmt));
    if (LWallGetCurrent(path, ARRAYSIZE(path), &style))
    {
        WCHAR *name = wcsrchr(path, L'\\');

        StringCchPrintfW(text, ARRAYSIZE(text), fmt, path[0] ? (name ? name + 1 : path) : L"-");
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)style, 0);
    }
    else
    {
        StringCchPrintfW(text, ARRAYSIZE(text), fmt, L"-");
    }
    SetDlgItemTextW(hwnd, IDC_WALL_CURRENT, text);
}

static INT_PTR CALLBACK
PersonalizeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PERSDATA *pd = (PERSDATA *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            static const UINT styleIds[LWALL_STYLE_COUNT] =
                { IDS_STYLE_FILL, IDS_STYLE_FIT, IDS_STYLE_STRETCH, IDS_STYLE_TILE, IDS_STYLE_CENTER };
            HWND list = GetDlgItem(hwnd, IDC_WALL_LIST), combo = GetDlgItem(hwnd, IDC_WALL_STYLE);
            LVCOLUMNW col;
            RECT rc;
            WCHAR current[MAX_PATH] = L"", name[64];
            LWALL_STYLE style;
            int i;

            pd = (PERSDATA *)LocalAlloc(LPTR, sizeof(*pd));
            if (!pd)
                return FALSE;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)pd);
            pd->Selected = -1;
            pd->Count = LWallEnum(pd->Items, LWALL_MAX_ITEMS);
            GetClientRect(list, &rc);
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_WIDTH;
            col.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL) - 4;
            SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
            SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
            for (i = 0; i < pd->Count; i++)
            {
                LVITEMW item;

                ZeroMemory(&item, sizeof(item));
                item.mask = LVIF_TEXT;
                item.iItem = i;
                item.pszText = pd->Items[i].Name;
                SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
            }
            for (i = 0; i < LWALL_STYLE_COUNT; i++)
            {
                LoadStr(styleIds[i], name, ARRAYSIZE(name));
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
            }
            SendMessageW(combo, CB_SETCURSEL, LWALL_FILL, 0);
            if (LWallGetCurrent(current, ARRAYSIZE(current), &style))
            {
                SendMessageW(combo, CB_SETCURSEL, (WPARAM)style, 0);
                for (i = 0; i < pd->Count; i++)
                {
                    if (!lstrcmpiW(pd->Items[i].Path, current))
                    {
                        LVITEMW item;

                        pd->Selected = i;
                        ZeroMemory(&item, sizeof(item));
                        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        item.state = LVIS_SELECTED | LVIS_FOCUSED;
                        SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&item);
                        break;
                    }
                }
            }
            if (!pd->Count)
                SetDlgItemTextW(hwnd, IDC_WALL_CURRENT, L"");
            EnableWindow(GetDlgItem(hwnd, IDC_WALL_APPLY), pd->Selected >= 0);
            UpdateCurrentLabel(hwnd);
            if (!pd->Count)
            {
                WCHAR text[160];

                LoadStr(IDS_NO_WALLPAPERS, text, ARRAYSIZE(text));
                SetDlgItemTextW(hwnd, IDC_WALL_CURRENT, text);
            }
            return TRUE;
        }
        case WM_NOTIFY:
        {
            NMLISTVIEW *nm = (NMLISTVIEW *)lParam;

            if (pd && nm->hdr.idFrom == IDC_WALL_LIST && nm->hdr.code == LVN_ITEMCHANGED &&
                (nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED))
            {
                pd->Selected = nm->iItem;
                EnableWindow(GetDlgItem(hwnd, IDC_WALL_APPLY), TRUE);
                InvalidateRect(GetDlgItem(hwnd, IDC_WALL_PREVIEW), NULL, TRUE);
            }
            return TRUE;
        }
        case WM_DRAWITEM:
            if (pd && wParam == IDC_WALL_PREVIEW)
            {
                PaintPreview(hwnd, (DRAWITEMSTRUCT *)lParam, pd);
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_WALL_APPLY && pd && pd->Selected >= 0 && pd->Selected < pd->Count)
            {
                int style = (int)SendDlgItemMessageW(hwnd, IDC_WALL_STYLE, CB_GETCURSEL, 0, 0);

                if (!LWallSet(pd->Items[pd->Selected].Path, (LWALL_STYLE)style))
                    Msg(hwnd, IDS_WALL_FAILED, MB_OK | MB_ICONERROR);
                UpdateCurrentLabel(hwnd);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (pd)
                LocalFree(pd);
            return TRUE;
    }
    return FALSE;
}

/* DISPLAY PAGE *************************************************************/

static void
FillResolutions(HWND hwnd, DISPDATA *dd)
{
    HWND combo = GetDlgItem(hwnd, IDC_DISP_RES);
    WCHAR fmt[32], text[64];
    int i, j, sel = -1;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    dd->SizeCount = 0;
    LoadStr(IDS_MODE_FORMAT, fmt, ARRAYSIZE(fmt));
    for (i = 0; i < dd->ModeCount; i++)
    {
        BOOL known = FALSE;

        if (dd->HaveCurrent && dd->Modes[i].Bits != dd->Current.Bits)
            continue;
        for (j = 0; j < dd->SizeCount && !known; j++)
            known = dd->Sizes[j].Width == dd->Modes[i].Width && dd->Sizes[j].Height == dd->Modes[i].Height;
        if (!known)
            dd->Sizes[dd->SizeCount++] = dd->Modes[i];
    }
    /* sort by area, largest first */
    for (i = 1; i < dd->SizeCount; i++)
    {
        LDISP_MODE key = dd->Sizes[i];

        for (j = i - 1; j >= 0 && (__int64)dd->Sizes[j].Width * dd->Sizes[j].Height <
                                      (__int64)key.Width * key.Height; j--)
            dd->Sizes[j + 1] = dd->Sizes[j];
        dd->Sizes[j + 1] = key;
    }
    for (i = 0; i < dd->SizeCount; i++)
    {
        StringCchPrintfW(text, ARRAYSIZE(text), fmt, dd->Sizes[i].Width, dd->Sizes[i].Height);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
        if (dd->HaveCurrent && dd->Sizes[i].Width == dd->Current.Width && dd->Sizes[i].Height == dd->Current.Height)
            sel = i;
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

static void
FillFrequencies(HWND hwnd, DISPDATA *dd)
{
    HWND combo = GetDlgItem(hwnd, IDC_DISP_HZ);
    int res = (int)SendDlgItemMessageW(hwnd, IDC_DISP_RES, CB_GETCURSEL, 0, 0);
    WCHAR fmt[32], text[64];
    int i, j, sel = -1;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    dd->FreqCount = 0;
    if (res < 0 || res >= dd->SizeCount)
        return;
    for (i = 0; i < dd->ModeCount && dd->FreqCount < (int)ARRAYSIZE(dd->Freqs); i++)
    {
        BOOL known = FALSE;

        if (dd->Modes[i].Width != dd->Sizes[res].Width || dd->Modes[i].Height != dd->Sizes[res].Height ||
            (dd->HaveCurrent && dd->Modes[i].Bits != dd->Current.Bits))
            continue;
        for (j = 0; j < dd->FreqCount && !known; j++)
            known = dd->Freqs[j] == dd->Modes[i].Frequency;
        if (!known)
            dd->Freqs[dd->FreqCount++] = dd->Modes[i].Frequency;
    }
    for (i = 1; i < dd->FreqCount; i++)
    {
        DWORD key = dd->Freqs[i];

        for (j = i - 1; j >= 0 && dd->Freqs[j] > key; j--)
            dd->Freqs[j + 1] = dd->Freqs[j];
        dd->Freqs[j + 1] = key;
    }
    LoadStr(IDS_HZ_FORMAT, fmt, ARRAYSIZE(fmt));
    for (i = 0; i < dd->FreqCount; i++)
    {
        if (dd->Freqs[i] <= 1)
            LoadStr(IDS_HZ_DEFAULT, text, ARRAYSIZE(text));
        else
            StringCchPrintfW(text, ARRAYSIZE(text), fmt, dd->Freqs[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
        if (dd->HaveCurrent && dd->Freqs[i] == dd->Current.Frequency)
            sel = i;
    }
    if (sel < 0 && dd->FreqCount)
        sel = dd->FreqCount - 1;
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

static void
ShowInfo(HWND hwnd, DISPDATA *dd, int index)
{
    WCHAR fmt[256], text[768], none[64];
    LDISP_MONITOR *m;

    if (index < 0 || index >= dd->MonitorCount)
    {
        SetDlgItemTextW(hwnd, IDC_DISP_INFO, L"");
        return;
    }
    m = &dd->Monitors[index];
    LoadStr(IDS_MONITOR_DETACHED, none, ARRAYSIZE(none));
    if (!m->Attached || !dd->HaveCurrent)
    {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s\n%s", m->Adapter, none);
    }
    else
    {
        LoadStr(IDS_INFO_FORMAT, fmt, ARRAYSIZE(fmt));
        StringCchPrintfW(text, ARRAYSIZE(text), fmt, m->Adapter, m->Monitor[0] ? m->Monitor : L"-",
                         dd->Current.Width, dd->Current.Height, dd->Current.Bits, dd->Current.Frequency,
                         m->Bounds.left, m->Bounds.top, LDispGetSystemDpi() * 100 / 96);
    }
    SetDlgItemTextW(hwnd, IDC_DISP_INFO, text);
}

static void
FillOrientation(HWND hwnd, DISPDATA *dd)
{
    static const UINT ids[4] = { IDS_CP_ORIENT_0, IDS_CP_ORIENT_1, IDS_CP_ORIENT_2, IDS_CP_ORIENT_3 };
    WCHAR text[64];
    DWORD o = dd->HaveCurrent ? dd->Current.Orientation : 0;

    SendDlgItemMessageW(hwnd, IDC_DISP_ORIENT, CB_RESETCONTENT, 0, 0);
    if (!dd->HaveCurrent)
        return;
    LoadStr(ids[o < 4 ? o : 0], text, ARRAYSIZE(text));
    SendDlgItemMessageW(hwnd, IDC_DISP_ORIENT, CB_ADDSTRING, 0, (LPARAM)text);
    SendDlgItemMessageW(hwnd, IDC_DISP_ORIENT, CB_SETCURSEL, 0, 0);
}

static void
PaintMonitors(HWND hwnd, DRAWITEMSTRUCT *dis, DISPDATA *dd)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem, bounds = { 0, 0, 0, 0 };
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = SelectObject(hdc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    int i, sel = (int)SendDlgItemMessageW(hwnd, IDC_DISP_MONITOR, CB_GETCURSEL, 0, 0), n = 0;
    LONG bw, bh, aw = rc.right - rc.left - 24, ah = rc.bottom - rc.top - 16, scale1000;

    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    ZeroMemory(dd->MonRect, sizeof(dd->MonRect));
    for (i = 0; i < dd->MonitorCount; i++)
    {
        if (!dd->Monitors[i].Attached)
            continue;
        if (!n++)
            bounds = dd->Monitors[i].Bounds;
        else
            UnionRect(&bounds, &bounds, &dd->Monitors[i].Bounds);
    }
    bw = bounds.right - bounds.left;
    bh = bounds.bottom - bounds.top;
    if (n && bw > 0 && bh > 0)
    {
        scale1000 = min(aw * 1000 / bw, ah * 1000 / bh);
        if (scale1000 < 1)
            scale1000 = 1;
        for (i = 0; i < dd->MonitorCount; i++)
        {
            RECT m, t;
            WCHAR num[8];
            HBRUSH fill = CreateSolidBrush(i == sel ? RGB(52, 104, 168) : RGB(120, 140, 170));
            HBRUSH edge = CreateSolidBrush(i == sel ? RGB(20, 60, 120) : RGB(70, 85, 110));

            if (!dd->Monitors[i].Attached)
            {
                DeleteObject(fill);
                DeleteObject(edge);
                continue;
            }
            m.left = rc.left + 12 + (dd->Monitors[i].Bounds.left - bounds.left) * scale1000 / 1000;
            m.top = rc.top + 8 + (dd->Monitors[i].Bounds.top - bounds.top) * scale1000 / 1000;
            m.right = m.left + (dd->Monitors[i].Bounds.right - dd->Monitors[i].Bounds.left) * scale1000 / 1000;
            m.bottom = m.top + (dd->Monitors[i].Bounds.bottom - dd->Monitors[i].Bounds.top) * scale1000 / 1000;
            dd->MonRect[i] = m;
            FillRect(hdc, &m, fill);
            FrameRect(hdc, &m, edge);
            StringCchPrintfW(num, ARRAYSIZE(num), L"%u", (UINT)(i + 1));
            t = m;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, num, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(fill);
            DeleteObject(edge);
        }
    }
    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK
IdentifyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            WCHAR num[8];
            HFONT font;
            HGDIOBJ old;

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            font = CreateFontW(-(rc.bottom - 40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Tahoma");
            old = SelectObject(hdc, font);
            StringCchPrintfW(num, ARRAYSIZE(num), L"%u", (UINT)(UINT_PTR)GetPropW(hwnd, L"num"));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, num, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            RemovePropW(hwnd, L"num");
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* A numbered window on every attached display for two seconds, like the Identify button of the system pages. */
static void
IdentifyDisplays(DISPDATA *dd)
{
    WNDCLASSW wc;
    int i;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = IdentifyProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"LeewayDisplayIdentify";
    RegisterClassW(&wc);
    for (i = 0; i < dd->MonitorCount; i++)
    {
        const RECT *b = &dd->Monitors[i].Bounds;
        int size = 160;
        HWND w;

        if (!dd->Monitors[i].Attached)
            continue;
        w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                            (b->left + b->right - size) / 2, (b->top + b->bottom - size) / 2, size, size,
                            NULL, NULL, hInst, NULL);
        if (!w)
            continue;
        SetPropW(w, L"num", (HANDLE)(UINT_PTR)(i + 1));
        ShowWindow(w, SW_SHOWNOACTIVATE);
        SetTimer(w, 1, 2000, NULL);
    }
}

static void
SelectMonitor(HWND hwnd, DISPDATA *dd, int index)
{
    BOOL usable = index >= 0 && index < dd->MonitorCount && dd->Monitors[index].Attached;

    dd->ModeCount = 0;
    dd->HaveCurrent = FALSE;
    if (usable)
    {
        dd->ModeCount = LDispEnumModes(dd->Monitors[index].Device, dd->Modes, LDISP_MAX_MODES);
        dd->HaveCurrent = LDispGetCurrent(dd->Monitors[index].Device, &dd->Current);
    }
    FillResolutions(hwnd, dd);
    FillFrequencies(hwnd, dd);
    ShowInfo(hwnd, dd, index);
    FillOrientation(hwnd, dd);
    InvalidateRect(GetDlgItem(hwnd, IDC_DISP_PICTURE), NULL, TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_DISP_RES), usable && (Caps() & LDISP_CAP_RESOLUTION));
    EnableWindow(GetDlgItem(hwnd, IDC_DISP_HZ), usable && (Caps() & LDISP_CAP_REFRESH));
    EnableWindow(GetDlgItem(hwnd, IDC_DISP_APPLY), usable);
}

typedef struct KEEPDATA
{
    int Remaining;
} KEEPDATA;

static void
KeepText(HWND hwnd, int remaining)
{
    WCHAR fmt[160], text[256];

    LoadStr(IDS_KEEP_TEXT, fmt, ARRAYSIZE(fmt));
    StringCchPrintfW(text, ARRAYSIZE(text), fmt, remaining);
    SetDlgItemTextW(hwnd, IDC_KEEP_TEXT, text);
}

static INT_PTR CALLBACK
KeepProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    KEEPDATA *kd = (KEEPDATA *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
        case WM_INITDIALOG:
            kd = (KEEPDATA *)lParam;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)kd);
            kd->Remaining = KEEP_SECONDS;
            KeepText(hwnd, kd->Remaining);
            SetTimer(hwnd, TIMER_KEEP, 1000, NULL);
            SetForegroundWindow(hwnd);
            return TRUE;
        case WM_TIMER:
            if (kd && wParam == TIMER_KEEP)
            {
                if (--kd->Remaining <= 0)
                {
                    KillTimer(hwnd, TIMER_KEEP);
                    EndDialog(hwnd, IDCANCEL);   /* automatic revert */
                }
                else
                {
                    KeepText(hwnd, kd->Remaining);
                }
            }
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                KillTimer(hwnd, TIMER_KEEP);
                EndDialog(hwnd, LOWORD(wParam));
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static void
ApplyResolution(HWND hwnd, DISPDATA *dd)
{
    int mon = (int)SendDlgItemMessageW(hwnd, IDC_DISP_MONITOR, CB_GETCURSEL, 0, 0);
    int res = (int)SendDlgItemMessageW(hwnd, IDC_DISP_RES, CB_GETCURSEL, 0, 0);
    int hz = (int)SendDlgItemMessageW(hwnd, IDC_DISP_HZ, CB_GETCURSEL, 0, 0);
    LDISP_MODE wanted, previous;
    KEEPDATA kd;
    LONG result;
    const WCHAR *device;

    if (mon < 0 || mon >= dd->MonitorCount || res < 0 || res >= dd->SizeCount || hz < 0 || hz >= dd->FreqCount ||
        !dd->HaveCurrent)
        return;
    device = dd->Monitors[mon].Device;
    previous = dd->Current;
    wanted = previous;
    wanted.Width = dd->Sizes[res].Width;
    wanted.Height = dd->Sizes[res].Height;
    wanted.Frequency = dd->Freqs[hz];
    if (wanted.Width == previous.Width && wanted.Height == previous.Height && wanted.Frequency == previous.Frequency)
        return;

    result = LDispTestMode(device, &wanted);           /* CDS_TEST: nothing changes yet */
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        Msg(hwnd, IDS_APPLY_UNSUPPORTED, MB_OK | MB_ICONWARNING);
        return;
    }
    result = LDispApplyMode(device, &wanted);
    if (result == DISP_CHANGE_RESTART)
    {
        Msg(hwnd, IDS_APPLY_RESTART, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        Msg(hwnd, IDS_APPLY_FAILED, MB_OK | MB_ICONERROR);
        return;
    }
    if (DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_KEEP), hwnd, KeepProc, (LPARAM)&kd) == IDOK)
    {
        /* only now is the mode written to the registry */
        if (LDispCommitMode(device, &wanted) != DISP_CHANGE_SUCCESSFUL)
            Msg(hwnd, IDS_APPLY_FAILED, MB_OK | MB_ICONERROR);
    }
    else if (LDispApplyMode(device, &previous) != DISP_CHANGE_SUCCESSFUL)
    {
        Msg(hwnd, IDS_APPLY_FAILED, MB_OK | MB_ICONERROR);
    }
    SelectMonitor(hwnd, dd, mon);
}

INT_PTR CALLBACK
DisplayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DISPDATA *dd = (DISPDATA *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            HWND combo = GetDlgItem(hwnd, IDC_DISP_MONITOR);
            WCHAR fmt[32], text[300], limits[160];
            int i, primary = 0;

            dd = (DISPDATA *)LocalAlloc(LPTR, sizeof(*dd));
            if (!dd)
                return FALSE;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)dd);
            dd->MonitorCount = LDispEnumMonitors(dd->Monitors, LDISP_MAX_MONITORS);
            LoadStr(IDS_MONITOR_FORMAT, fmt, ARRAYSIZE(fmt));
            for (i = 0; i < dd->MonitorCount; i++)
            {
                StringCchPrintfW(text, ARRAYSIZE(text), fmt, (UINT)(i + 1), dd->Monitors[i].Adapter);
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
                if (dd->Monitors[i].Primary)
                    primary = i;
            }
            if (!dd->MonitorCount)
            {
                LoadStr(IDS_NO_MONITORS, text, ARRAYSIZE(text));
                SetDlgItemTextW(hwnd, IDC_DISP_INFO, text);
            }
            SendMessageW(combo, CB_SETCURSEL, dd->MonitorCount ? primary : -1, 0);
            SelectMonitor(hwnd, dd, dd->MonitorCount ? primary : -1);
            LoadStr(IDS_CAPS_LIMITS, limits, ARRAYSIZE(limits));
            SetDlgItemTextW(hwnd, IDC_DISP_LIMITS, limits);
            return TRUE;
        }
        case WM_COMMAND:
            if (!dd)
                break;
            switch (LOWORD(wParam))
            {
                case IDC_DISP_MONITOR:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        SelectMonitor(hwnd, dd, (int)SendDlgItemMessageW(hwnd, IDC_DISP_MONITOR, CB_GETCURSEL, 0, 0));
                    return TRUE;
                case IDC_DISP_RES:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        FillFrequencies(hwnd, dd);
                    return TRUE;
                case IDC_DISP_APPLY:
                    ApplyResolution(hwnd, dd);
                    return TRUE;
                case IDOK:
                    ApplyResolution(hwnd, dd);
                    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_DISP_DONE, 0), 0);
                    return TRUE;
                case IDCANCEL:
                    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_DISP_DONE, 0), 0);
                    return TRUE;
                case IDC_DISP_IDENTIFY:
                    IdentifyDisplays(dd);
                    return TRUE;
                case IDC_DISP_ADV:
                {
                    HINSTANCE r = ShellExecuteW(hwnd, L"open", L"rundll32.exe",
                                                L"shell32.dll,Control_RunDLL desk.cpl,,3", NULL, SW_SHOWNORMAL);

                    if ((UINT_PTR)r <= 32)
                        Msg(hwnd, IDS_CP_OPEN_FAILED, MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                case IDC_DISP_PICTURE:
                    if (HIWORD(wParam) == STN_CLICKED)
                    {
                        POINT pt;
                        int i;

                        GetCursorPos(&pt);
                        ScreenToClient(GetDlgItem(hwnd, IDC_DISP_PICTURE), &pt);
                        for (i = 0; i < dd->MonitorCount; i++)
                        {
                            if (PtInRect(&dd->MonRect[i], pt))
                            {
                                SendDlgItemMessageW(hwnd, IDC_DISP_MONITOR, CB_SETCURSEL, (WPARAM)i, 0);
                                SelectMonitor(hwnd, dd, i);
                                break;
                            }
                        }
                    }
                    return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (dd && wParam == IDC_DISP_PICTURE)
            {
                PaintMonitors(hwnd, (DRAWITEMSTRUCT *)lParam, dd);
                return TRUE;
            }
            break;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
            /* only the Control Panel page is white; the tab of the classic dialog keeps the tab colour */
            if (GetDlgItem(hwnd, IDC_DISP_PICTURE))
            {
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (INT_PTR)GetStockObject(WHITE_BRUSH);
            }
            break;
        case WM_DESTROY:
            if (dd)
                LocalFree(dd);
            return TRUE;
    }
    return FALSE;
}

/* SCREEN SAVER PAGE ********************************************************/

static const UINT SaverMinutes[] = { 1, 2, 3, 5, 10, 15, 20, 30, 60 };

typedef struct SAVERDATA
{
    LSAVER_ITEM Items[LSAVER_MAX_ITEMS];
    int Count;
    UINT Waits[ARRAYSIZE(SaverMinutes) + 1];  /* seconds, one per combobox row */
    int WaitCount;
} SAVERDATA;

static BOOL
SamePath(const WCHAR *a, const WCHAR *b)
{
    WCHAR la[MAX_PATH], lb[MAX_PATH];

    if (!GetLongPathNameW(a, la, ARRAYSIZE(la)))
        lstrcpynW(la, a, ARRAYSIZE(la));
    if (!GetLongPathNameW(b, lb, ARRAYSIZE(lb)))
        lstrcpynW(lb, b, ARRAYSIZE(lb));
    return !lstrcmpiW(la, lb);
}

static int
SaverSelected(HWND hwnd)
{
    return (int)SendDlgItemMessageW(hwnd, IDC_SAVER_LIST, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void
SaverEnableButtons(HWND hwnd)
{
    BOOL hasSaver = SaverSelected(hwnd) > 0;

    EnableWindow(GetDlgItem(hwnd, IDC_SAVER_TEST), hasSaver);
    EnableWindow(GetDlgItem(hwnd, IDC_SAVER_WAIT), hasSaver);
}

static void
SaverFill(HWND hwnd, SAVERDATA *sd)
{
    HWND list = GetDlgItem(hwnd, IDC_SAVER_LIST);
    WCHAR path[MAX_PATH], text[64];
    UINT timeout = 600;
    BOOL active = FALSE;
    LVITEMW item;
    LVCOLUMNW col;
    RECT rc;
    int i, select = 0, wait = 0;

    GetClientRect(list, &rc);
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL) - 4;
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

    if (!(Caps() & LDISP_CAP_SCREENSAVER) || !LSaverGet(path, ARRAYSIZE(path), &timeout, &active))
    {
        WCHAR msg[160];

        LoadStr(IDS_SAVER_UNAVAILABLE, msg, ARRAYSIZE(msg));
        SetDlgItemTextW(hwnd, IDC_SAVER_NOTE, msg);
        EnableWindow(list, FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVER_APPLY), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVER_WAIT), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_SAVER_TEST), FALSE);
        return;
    }
    sd->Count = LSaverEnum(sd->Items, LSAVER_MAX_ITEMS - 1);
    /* A saver outside System32 (or spelled differently) is shown as an extra row, never as (None). */
    if (path[0])
    {
        for (i = 0; i < sd->Count && !SamePath(path, sd->Items[i].Path); i++)
            ;
        if (i == sd->Count)
        {
            const WCHAR *base = wcsrchr(path, L'\\');
            WCHAR *dot;

            lstrcpynW(sd->Items[i].Path, path, ARRAYSIZE(sd->Items[i].Path));
            lstrcpynW(sd->Items[i].Name, base ? base + 1 : path, ARRAYSIZE(sd->Items[i].Name));
            dot = wcsrchr(sd->Items[i].Name, L'.');
            if (dot)
                *dot = 0;
            sd->Count++;
        }
    }
    LoadStr(IDS_SAVER_NONE, text, ARRAYSIZE(text));
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.pszText = text;
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    for (i = 0; i < sd->Count; i++)
    {
        item.iItem = i + 1;
        item.pszText = sd->Items[i].Name;
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (path[0] && SamePath(path, sd->Items[i].Path))
            select = i + 1;
    }
    /* The combobox holds the fixed steps plus the current value when it is none of them, so Apply
     * never rewrites a timeout the user did not change. */
    for (i = 0; i < (int)ARRAYSIZE(SaverMinutes); i++)
    {
        sd->Waits[sd->WaitCount++] = SaverMinutes[i] * 60;
    }
    {
        BOOL found = FALSE;

        for (i = 0; i < sd->WaitCount; i++)
            found |= sd->Waits[i] == timeout;
        if (!found && timeout >= 60)
        {
            for (i = sd->WaitCount; i > 0 && sd->Waits[i - 1] > timeout; i--)
                sd->Waits[i] = sd->Waits[i - 1];
            sd->Waits[i] = timeout;
            sd->WaitCount++;
        }
    }
    for (i = 0; i < sd->WaitCount; i++)
    {
        WCHAR fmt[32];

        LoadStr(IDS_SAVER_MINUTES, fmt, ARRAYSIZE(fmt));
        StringCchPrintfW(text, ARRAYSIZE(text), fmt, (sd->Waits[i] + 59) / 60);
        SendDlgItemMessageW(hwnd, IDC_SAVER_WAIT, CB_ADDSTRING, 0, (LPARAM)text);
        if (sd->Waits[i] == timeout)
            wait = i;
    }
    SendDlgItemMessageW(hwnd, IDC_SAVER_WAIT, CB_SETCURSEL, (WPARAM)wait, 0);
    item.mask = LVIF_STATE;
    item.iItem = select;
    item.state = item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)select, (LPARAM)&item);
    SaverEnableButtons(hwnd);
}

INT_PTR CALLBACK
SaverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SAVERDATA *sd = (SAVERDATA *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
        case WM_INITDIALOG:
            sd = (SAVERDATA *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sd));
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)sd);
            if (!sd)
                return TRUE;
            SendDlgItemMessageW(hwnd, IDC_SAVER_LIST, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
            SaverFill(hwnd, sd);
            return TRUE;
        case WM_DESTROY:
            if (sd)
                HeapFree(GetProcessHeap(), 0, sd);
            SetWindowLongPtrW(hwnd, DWLP_USER, 0);
            break;
        case WM_NOTIFY:
            if (((NMHDR *)lParam)->idFrom == IDC_SAVER_LIST && ((NMHDR *)lParam)->code == LVN_ITEMCHANGED)
                SaverEnableButtons(hwnd);
            break;
        case WM_COMMAND:
        {
            int sel = SaverSelected(hwnd);

            if (!sd)
                break;
            if (LOWORD(wParam) == IDC_SAVER_TEST && sel > 0 && sel <= sd->Count)
            {
                if (!LSaverTest(sd->Items[sel - 1].Path))
                    Msg(hwnd, IDS_SAVER_FAILED, MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_SAVER_APPLY)
            {
                int w = (int)SendDlgItemMessageW(hwnd, IDC_SAVER_WAIT, CB_GETCURSEL, 0, 0);
                UINT secs = (w >= 0 && w < sd->WaitCount) ? sd->Waits[w] : 600;
                const WCHAR *path = (sel > 0 && sel <= sd->Count) ? sd->Items[sel - 1].Path : NULL;

                if (!LSaverSet(path, secs, path != NULL))
                    Msg(hwnd, IDS_SAVER_FAILED, MB_OK | MB_ICONERROR);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

/* GRAPHICS PAGE **********************************************************/
/* Writes HKCU\Software\Wine\Direct3D, the keys wined3d actually reads.
 * "Default" removes the value so the engine keeps its built-in behaviour.
 * A stored value the page does not offer is shown read-only as "Custom (...)",
 * and only combos the user actually changes are written, so nothing is wiped. */
#define GFX_D3D_KEY L"Software\\Wine\\Direct3D"

typedef struct GFX_OPT
{
    const WCHAR *label;   /* NULL = localized "Default"; selecting it removes the value */
    int type;             /* 0 remove, 1 REG_DWORD, 2 REG_SZ */
    DWORD dword;
    const WCHAR *sz;
} GFX_OPT;

/* Vulkan is omitted until its runtime init is verified on our target (needs a vulkan-1.dll ICD). */
static const GFX_OPT gfxRenderer[] = {
    { NULL,      0, 0, NULL },
    { L"OpenGL", 2, 0, L"gl" },
    { L"No 3D",  2, 0, L"no3d" },
};
/* shader_backend applies to the OpenGL path only; the engine honours just these two. */
static const GFX_OPT gfxShader[] = {
    { NULL,                   0, 0, NULL },
    { L"GLSL",                2, 0, L"glsl" },
    { L"GLSL (vkd3d-shader)", 2, 0, L"glsl-vkd3d" },
};
static const GFX_OPT gfxLevel[] = {
    { NULL,   0, 0, NULL },
    { L"1.4", 1, 1, NULL },
    { L"2",   1, 2, NULL },
    { L"3",   1, 3, NULL },
};
static const GFX_OPT gfxVidMem[] = {
    { NULL,      0, 0, NULL },
    { L"8 MB",   2, 0, L"8 MB" },
    { L"16 MB",  2, 0, L"16 MB" },
    { L"32 MB",  2, 0, L"32 MB" },
    { L"64 MB",  2, 0, L"64 MB" },
    { L"128 MB", 2, 0, L"128 MB" },
    { L"256 MB", 2, 0, L"256 MB" },
    { L"512 MB", 2, 0, L"512 MB" },
};

typedef struct GFX_CTRL
{
    int id;
    const WCHAR *value;
    const GFX_OPT *opt;
    int count;
    BOOL icase;           /* case-insensitive match, as wined3d reads shader_backend (stricmp) */
} GFX_CTRL;

static const GFX_CTRL gfxCtrls[] = {
    { IDC_GFX_RENDERER, L"renderer",         gfxRenderer, ARRAYSIZE(gfxRenderer), FALSE },
    { IDC_GFX_SHADER,   L"shader_backend",   gfxShader,   ARRAYSIZE(gfxShader),   TRUE  },
    { IDC_GFX_VIDMEM,   L"VideoMemorySize",  gfxVidMem,   ARRAYSIZE(gfxVidMem),   FALSE },
    { IDC_GFX_GSLEVEL,  L"MaxShaderModelGS", gfxLevel,    ARRAYSIZE(gfxLevel),    FALSE },
    { IDC_GFX_PSLEVEL,  L"MaxShaderModelPS", gfxLevel,    ARRAYSIZE(gfxLevel),    FALSE },
    { IDC_GFX_VSLEVEL,  L"MaxShaderModelVS", gfxLevel,    ARRAYSIZE(gfxLevel),    FALSE },
};
#define GFX_NCTRL ((int)ARRAYSIZE(gfxCtrls))

static void
GfxInitCombo(HWND hwnd, HKEY hKey, const GFX_CTRL *c)
{
    union { DWORD dw; WCHAR sz[128]; BYTE raw[256]; } buf;
    DWORD cb = sizeof(buf) - sizeof(WCHAR), type = 0;
    WCHAR def[64];
    BOOL have, matched = FALSE;
    int i, sel = 0;

    have = (hKey != NULL &&
            RegQueryValueExW(hKey, c->value, NULL, &type, buf.raw, &cb) == ERROR_SUCCESS);
    if (have && type == REG_SZ)
        buf.sz[cb / sizeof(WCHAR)] = 0;   /* guarantee NUL termination before compare */
    LoadStr(IDS_GFX_DEFAULT, def, ARRAYSIZE(def));
    for (i = 0; i < c->count; i++)
    {
        const WCHAR *txt = c->opt[i].label ? c->opt[i].label : def;

        SendDlgItemMessageW(hwnd, c->id, CB_ADDSTRING, 0, (LPARAM)txt);
        if (have && i > 0 && !matched)
        {
            if (c->opt[i].type == 1 && type == REG_DWORD && buf.dw == c->opt[i].dword)
            {
                sel = i;
                matched = TRUE;
            }
            else if (c->opt[i].type == 2 && type == REG_SZ &&
                     (c->icase ? !_wcsicmp(buf.sz, c->opt[i].sz) : !wcscmp(buf.sz, c->opt[i].sz)))
            {
                sel = i;
                matched = TRUE;
            }
        }
    }
    if (have && !matched)
    {
        WCHAR item[160], valstr[132];

        if (type == REG_DWORD)
            StringCchPrintfW(valstr, ARRAYSIZE(valstr), L"%lu", (unsigned long)buf.dw);
        else if (type == REG_SZ)
            StringCchCopyW(valstr, ARRAYSIZE(valstr), buf.sz);
        else
            valstr[0] = 0;
        StringCchPrintfW(item, ARRAYSIZE(item), L"Custom (%s)", valstr);
        sel = (int)SendDlgItemMessageW(hwnd, c->id, CB_ADDSTRING, 0, (LPARAM)item);
    }
    SendDlgItemMessageW(hwnd, c->id, CB_SETCURSEL, sel, 0);
}

static void
GfxSaveCombo(HWND hwnd, HKEY hKey, const GFX_CTRL *c)
{
    int sel = (int)SendDlgItemMessageW(hwnd, c->id, CB_GETCURSEL, 0, 0);

    if (sel < 0 || sel >= c->count)   /* "Custom (...)" or none: leave the stored value alone */
        return;
    if (c->opt[sel].type == 0)
    {
        RegDeleteValueW(hKey, c->value);
    }
    else if (c->opt[sel].type == 1)
    {
        DWORD v = c->opt[sel].dword;
        RegSetValueExW(hKey, c->value, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    }
    else if (c->opt[sel].type == 2)
    {
        RegSetValueExW(hKey, c->value, 0, REG_SZ, (const BYTE *)c->opt[sel].sz,
                       (DWORD)((wcslen(c->opt[sel].sz) + 1) * sizeof(WCHAR)));
    }
}

static void
GfxUpdateEnable(HWND hwnd)
{
    /* The shader backend only exists for the OpenGL path; "No 3D" (index 2) has none. */
    int r = (int)SendDlgItemMessageW(hwnd, IDC_GFX_RENDERER, CB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(hwnd, IDC_GFX_SHADER), r != 2);
}

static INT_PTR CALLBACK
GraphicsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LONG_PTR dirty = GetWindowLongPtrW(hwnd, DWLP_USER);
    HKEY hKey;
    int i;

    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
        case WM_INITDIALOG:
            SetWindowLongPtrW(hwnd, DWLP_USER, 0);
            if (RegCreateKeyExW(HKEY_CURRENT_USER, GFX_D3D_KEY, 0, NULL, 0, KEY_READ, NULL, &hKey, NULL)
                    != ERROR_SUCCESS)
                hKey = NULL;
            for (i = 0; i < GFX_NCTRL; i++)
                GfxInitCombo(hwnd, hKey, &gfxCtrls[i]);   /* populated even if the key is missing */
            if (hKey)
                RegCloseKey(hKey);
            GfxUpdateEnable(hwnd);
            EnableWindow(GetDlgItem(hwnd, IDC_GFX_APPLY), FALSE);   /* nothing changed yet */
            return TRUE;
        case WM_COMMAND:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                for (i = 0; i < GFX_NCTRL; i++)
                {
                    if (LOWORD(wParam) == gfxCtrls[i].id)
                    {
                        SetWindowLongPtrW(hwnd, DWLP_USER, dirty | ((LONG_PTR)1 << i));
                        EnableWindow(GetDlgItem(hwnd, IDC_GFX_APPLY), TRUE);
                        if (gfxCtrls[i].id == IDC_GFX_RENDERER)
                            GfxUpdateEnable(hwnd);
                        return TRUE;
                    }
                }
                break;
            }
            if (LOWORD(wParam) == IDC_GFX_APPLY)
            {
                if (RegCreateKeyExW(HKEY_CURRENT_USER, GFX_D3D_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL)
                        == ERROR_SUCCESS)
                {
                    for (i = 0; i < GFX_NCTRL; i++)
                    {
                        if ((dirty & ((LONG_PTR)1 << i)) &&
                            IsWindowEnabled(GetDlgItem(hwnd, gfxCtrls[i].id)))
                            GfxSaveCombo(hwnd, hKey, &gfxCtrls[i]);   /* only touched + active combos */
                    }
                    RegCloseKey(hKey);
                }
                SetWindowLongPtrW(hwnd, DWLP_USER, 0);
                EnableWindow(GetDlgItem(hwnd, IDC_GFX_APPLY), FALSE);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

/* MAIN WINDOW **************************************************************/

static void
ShowTab(MAINDATA *md, int tab)
{
    int i;

    for (i = 0; i < md->PageCount; i++)
        ShowWindow(md->Pages[i], i == tab ? SW_SHOW : SW_HIDE);
    SendMessageW(md->Tab, TCM_SETCURSEL, (WPARAM)tab, 0);
}

static INT_PTR CALLBACK
MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MAINDATA *md = (MAINDATA *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            static const UINT tabIds[4] = { IDS_TAB_PERSONALIZE, IDS_TAB_DISPLAY, IDS_TAB_GRAPHICS, IDS_TAB_SAVER };
            static const UINT pageIds[4] = { IDD_PERSONALIZE, IDD_DISPLAY, IDD_GRAPHICS, IDD_SAVER };
            static DLGPROC procs[4] = { PersonalizeProc, DisplayProc, GraphicsProc, SaverProc };
            WCHAR text[64];
            TCITEMW item;
            RECT rc;
            int i;

            md = (MAINDATA *)lParam;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)md);
            md->Tab = GetDlgItem(hwnd, IDC_TAB);
            /* The screen saver tab is offered only when the system answers its settings calls. */
            md->PageCount = (Caps() & LDISP_CAP_SCREENSAVER) ? 4 : 3;
            for (i = 0; i < md->PageCount; i++)
            {
                LoadStr(tabIds[i], text, ARRAYSIZE(text));
                ZeroMemory(&item, sizeof(item));
                item.mask = TCIF_TEXT;
                item.pszText = text;
                SendMessageW(md->Tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
            }
            GetClientRect(md->Tab, &rc);
            SendMessageW(md->Tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rc);
            MapWindowPoints(md->Tab, hwnd, (POINT *)&rc, 2);
            for (i = 0; i < md->PageCount; i++)
            {
                md->Pages[i] = CreateDialogParamW(hInst, MAKEINTRESOURCEW(pageIds[i]), hwnd, procs[i], 0);
                if (md->Pages[i])
                    SetWindowPos(md->Pages[i], HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                 SWP_NOACTIVATE);
            }
            LoadStr(IDS_TITLE, text, ARRAYSIZE(text));
            SetWindowTextW(hwnd, text);
            ShowTab(md, md->InitialTab);
            /* DialogBox shows the window from its modal loop and user32 ignores DS_SETFOREGROUND, so the
             * dialog came up inactive (Alt+F4 went elsewhere). The property sheet of desk.cpl is created
             * visible and does not have the problem. */
            PostMessageW(hwnd, WM_LDISP_ACTIVATE, 0, 0);
            return TRUE;
        }
        case WM_LDISP_ACTIVATE:
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            return TRUE;
        case WM_NOTIFY:
            if (md && ((NMHDR *)lParam)->idFrom == IDC_TAB && ((NMHDR *)lParam)->code == TCN_SELCHANGE)
            {
                ShowTab(md, (int)SendMessageW(md->Tab, TCM_GETCURSEL, 0, 0));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_DISP_DONE)
            {
                EndDialog(hwnd, 0);   /* OK or Cancel on the display tab */
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hwnd, 0);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static void
RunApplet(HWND parent, int tab)
{
    MAINDATA md;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    struct GdiplusStartupInput gdipStartup;

    CapsValid = FALSE;
    ZeroMemory(&md, sizeof(md));
    md.InitialTab = tab;
    InitCommonControlsEx(&icc);
    gdipStartup.GdiplusVersion = 1;
    gdipStartup.DebugEventCallback = NULL;
    gdipStartup.SuppressBackgroundThread = FALSE;
    gdipStartup.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&md.GdipToken, &gdipStartup, NULL);
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_MAIN), parent, MainProc, (LPARAM)&md);
    if (md.GdipToken)
        GdiplusShutdown(md.GdipToken);
}

/* CONTROL PANEL ENTRY ******************************************************/

LONG CALLBACK
CPlApplet(HWND hwndCPl, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
    UINT i = (UINT)lParam1;

    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;
        case CPL_GETCOUNT:
            return ARRAYSIZE(Applets);
        case CPL_INQUIRE:
        {
            CPLINFO *info = (CPLINFO *)lParam2;

            if (i >= ARRAYSIZE(Applets))
                return TRUE;
            info->idIcon = IDI_CPLICON;
            info->idName = Applets[i].NameId;
            info->idInfo = Applets[i].DescId;
            info->lData = 0;
            return TRUE;
        }
        case CPL_DBLCLK:
            if (i < ARRAYSIZE(Applets))
                RunApplet(hwndCPl, Applets[i].Tab);
            return TRUE;
        case CPL_STOP:
        case CPL_EXIT:
            return TRUE;
    }
    return 0;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
