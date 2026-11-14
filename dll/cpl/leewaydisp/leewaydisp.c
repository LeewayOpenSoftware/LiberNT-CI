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
#include <strsafe.h>
#include <gdiplus.h>
#include <leewayui/display_backend.h>
#include "resource.h"

#define KEEP_SECONDS 15
#define TIMER_KEEP   1

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

static HINSTANCE hInst;

static DWORD CapsValue;
static BOOL CapsValid;

/* One query per applet run; the backend probes the system on every call. */
static DWORD
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
    HWND Pages[3];
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
} DISPDATA;

/* HELPERS ******************************************************************/

static void
LoadStr(UINT id, WCHAR *buf, int cch)
{
    if (!LoadStringW(hInst, id, buf, cch))
        buf[0] = 0;
}

static void
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

static INT_PTR CALLBACK
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

static INT_PTR CALLBACK
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
            static const UINT tabIds[3] = { IDS_TAB_PERSONALIZE, IDS_TAB_DISPLAY, IDS_TAB_SAVER };
            static const UINT pageIds[3] = { IDD_PERSONALIZE, IDD_DISPLAY, IDD_SAVER };
            static DLGPROC procs[3] = { PersonalizeProc, DisplayProc, SaverProc };
            WCHAR text[64];
            TCITEMW item;
            RECT rc;
            int i;

            md = (MAINDATA *)lParam;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)md);
            md->Tab = GetDlgItem(hwnd, IDC_TAB);
            /* The screen saver tab is offered only when the system answers its settings calls. */
            md->PageCount = (Caps() & LDISP_CAP_SCREENSAVER) ? 3 : 2;
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
            return TRUE;
        }
        case WM_NOTIFY:
            if (md && ((NMHDR *)lParam)->idFrom == IDC_TAB && ((NMHDR *)lParam)->code == TCN_SELCHANGE)
            {
                ShowTab(md, (int)SendMessageW(md->Tab, TCM_GETCURSEL, 0, 0));
                return TRUE;
            }
            break;
        case WM_COMMAND:
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
