/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Personalization page components hosted in an Explorer view (LeewayPage* ABI)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <gdiplus.h>
#include <leewayui/display_backend.h>
#include "resource.h"
#include "internal.h"
#include "thumbgrid.h"
#include "pagehost.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define PAGE_CLASS  L"LeewayControlPanelPage"
#define MAX_HIST    32
#define MAX_LINKS   16
#define MAX_LOCS    8
#define WM_NAVIGATE (WM_APP + 2)

#define ID_GRID     501
#define ID_LOC      502
#define ID_BROWSE   503
#define ID_SELALL   504
#define ID_CLEARALL 505
#define ID_POS      506
#define ID_INTERVAL 507
#define ID_SHUFFLE  508
#define ID_SAVE     509
#define ID_CANCEL   510
#define ID_EMPTY    511
#define ID_NOTE     512

/* link actions */
enum
{
    ACT_NONE = 0,
    ACT_HOME,
    ACT_ICONS,
    ACT_POINTERS,
    ACT_ACCOUNT,
    ACT_DISPLAY,
    ACT_TASKBAR,
    ACT_EASE,
    ACT_SAVETHEME,
    ACT_MORETHEMES,
    ACT_BACKGROUND,
    ACT_COLOR,
    ACT_SOUNDS,
    ACT_SAVER,
    ACT_RESOLUTION,
    ACT_PERSONALIZE,
    ACT_ADVANCED
};

enum { LK_TEXT = 0, LK_TILE };
enum { TILE_BG = 0, TILE_COLOR, TILE_SOUNDS, TILE_SAVER };

typedef struct LINK
{
    RECT Rect;
    int Action;
    int Kind;
    int Tile;
    BOOL Enabled;
    WCHAR Text[96];
    WCHAR Value[96];
} LINK;

typedef struct LOCATION
{
    WCHAR Path[MAX_PATH];
    WCHAR Name[64];
} LOCATION;

typedef struct SHELLDATA SHELLDATA;

typedef struct PAGECREATE
{
    UINT Id;
    const LWPAGE_HOST *Host;
} PAGECREATE;

typedef struct PAGEDATA
{
    SHELLDATA *Shell;
    HWND Hwnd;
    LWPAGE_HOST Host;
    int Id;
    LINK Links[MAX_LINKS];
    int LinkCount;
    int Hot;
    HWND Grid, Loc, Browse, SelAll, ClearAll, Pos, Interval, Shuffle, Save, Cancel, Empty, Note, Child;
    LOCATION Locs[MAX_LOCS];
    int LocCount;
    HBITMAP BgThumb;
    int PaneWidth;
    COLORREF ColorSel;
    RECT Swatch[16];
} PAGEDATA;

struct SHELLDATA
{
    HFONT Font, LinkFont, HeadFont;
    int Dpi;
    BOOL Tracking;
    ULONG_PTR GdipToken;
    int Refs;
};

static SHELLDATA *G;     /* shared by the page components of one host, released with the last one */

#define S(v) MulDiv((v), G->Dpi, 96)

static const COLORREF ColLink = RGB(0, 51, 153);
static const COLORREF ColLinkHot = RGB(51, 153, 255);
static const COLORREF ColDisabled = RGB(150, 150, 150);
static const COLORREF ColHead = RGB(0, 51, 153);

static void Navigate(PAGEDATA *pd, UINT page);


/* HELPERS ******************************************************************/

static const WCHAR *
Str(UINT id)
{
    static WCHAR ring[8][160];
    static int next;
    WCHAR *b = ring[next++ & 7];

    LoadStr(id, b, 160);
    return b;
}

static void
Launch(HWND hwnd, const WCHAR *file, const WCHAR *params)
{
    HINSTANCE r = ShellExecuteW(hwnd, L"open", file, params, NULL, SW_SHOWNORMAL);

    if ((UINT_PTR)r <= 32)
        Msg(hwnd, IDS_CP_OPEN_FAILED, MB_OK | MB_ICONWARNING);
}

static void
FillGradient(HDC hdc, const RECT *rc, COLORREF top, COLORREF bottom)
{
    int h = rc->bottom - rc->top, i, step = 2;

    for (i = 0; i < h; i += step)
    {
        COLORREF c = RGB(GetRValue(top) + (GetRValue(bottom) - GetRValue(top)) * i / (h ? h : 1),
                         GetGValue(top) + (GetGValue(bottom) - GetGValue(top)) * i / (h ? h : 1),
                         GetBValue(top) + (GetBValue(bottom) - GetBValue(top)) * i / (h ? h : 1));
        RECT r = { rc->left, rc->top + i, rc->right, rc->top + i + step };
        HBRUSH b = CreateSolidBrush(c);

        if (r.bottom > rc->bottom)
            r.bottom = rc->bottom;
        FillRect(hdc, &r, b);
        DeleteObject(b);
    }
}

static void
FileLabel(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *base = wcsrchr(path, L'\\');
    WCHAR *dot;

    lstrcpynW(out, base ? base + 1 : path, cch);
    dot = wcsrchr(out, L'.');
    if (dot)
        *dot = 0;
}

static BOOL
SamePathCi(const WCHAR *a, const WCHAR *b)
{
    WCHAR la[MAX_PATH], lb[MAX_PATH];

    if (!GetLongPathNameW(a, la, ARRAYSIZE(la)))
        lstrcpynW(la, a, ARRAYSIZE(la));
    if (!GetLongPathNameW(b, lb, ARRAYSIZE(lb)))
        lstrcpynW(lb, b, ARRAYSIZE(lb));
    return !lstrcmpiW(la, lb);
}

/* LINKS ********************************************************************/

static LINK *
AddLink(PAGEDATA *pd, int action, int kind, BOOL enabled, UINT textId)
{
    LINK *l;

    if (pd->LinkCount >= MAX_LINKS)
        return NULL;
    l = &pd->Links[pd->LinkCount++];
    ZeroMemory(l, sizeof(*l));
    l->Action = action;
    l->Kind = kind;
    l->Enabled = enabled;
    lstrcpynW(l->Text, Str(textId), ARRAYSIZE(l->Text));
    return l;
}

static void
DrawTextLink(HDC hdc, PAGEDATA *pd, LINK *l, BOOL hot)
{
    SelectObject(hdc, hot && l->Enabled ? G->LinkFont : G->Font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, !l->Enabled ? ColDisabled : (hot ? ColLinkHot : ColLink));
    DrawTextW(hdc, l->Text, -1, &l->Rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    UNREFERENCED_PARAMETER(pd);
}

static void
RoundBox(HDC hdc, const RECT *r, COLORREF fill, COLORREF line)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);

    RoundRect(hdc, r->left, r->top, r->right, r->bottom, 6, 6);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void
DrawTileIcon(HDC hdc, PAGEDATA *pd, int tile, int cx, int top)
{
    int w = S(52), h = S(44);
    RECT r = { cx - w / 2, top, cx + w / 2, top + h };

    switch (tile)
    {
        case TILE_BG:
        {
            HBRUSH f = CreateSolidBrush(RGB(70, 80, 100));

            RoundBox(hdc, &r, RGB(235, 240, 248), RGB(120, 130, 150));
            InflateRect(&r, -3, -3);
            if (pd->BgThumb)
            {
                HDC mem = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(mem, pd->BgThumb);

                StretchBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, S(52) - 6, S(44) - 6, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            else
            {
                FillRect(hdc, &r, f);
            }
            DeleteObject(f);
            break;
        }
        case TILE_COLOR:
        {
            RECT sq = { cx - S(20), top + S(2), cx + S(20), top + S(42) };

            RoundBox(hdc, &sq, GetSysColor(COLOR_ACTIVECAPTION), RGB(255, 255, 255));
            InflateRect(&sq, 1, 1);
            RoundBox(hdc, &sq, GetSysColor(COLOR_ACTIVECAPTION), RGB(120, 130, 150));
            sq.left += 3; sq.top += 3; sq.right -= 3; sq.bottom = sq.top + S(12);
            {
                HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
                RECT hl = sq;

                hl.bottom = hl.top + 2;
                FillRect(hdc, &hl, b);
                DeleteObject(b);
            }
            break;
        }
        case TILE_SOUNDS:
        {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(40, 60, 100));
            HBRUSH br = CreateSolidBrush(RGB(40, 60, 100));
            HGDIOBJ op = SelectObject(hdc, pen), ob = SelectObject(hdc, br);
            int x1 = cx - S(14), x2 = cx + S(8), yb = top + S(34);

            Ellipse(hdc, x1 - S(6), yb - S(4), x1 + S(4), yb + S(4));
            Ellipse(hdc, x2 - S(6), yb - S(10), x2 + S(4), yb - S(2));
            MoveToEx(hdc, x1 + S(3), yb, NULL); LineTo(hdc, x1 + S(3), top + S(8));
            MoveToEx(hdc, x2 + S(3), yb - S(6), NULL); LineTo(hdc, x2 + S(3), top + S(4));
            MoveToEx(hdc, x1 + S(3), top + S(8), NULL); LineTo(hdc, x2 + S(3), top + S(4));
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(pen);
            DeleteObject(br);
            break;
        }
        case TILE_SAVER:
        {
            RECT scr = { cx - S(22), top + S(2), cx + S(22), top + S(32) };
            HBRUSH nav = CreateSolidBrush(RGB(20, 30, 70)), moon = CreateSolidBrush(RGB(240, 240, 200));
            HGDIOBJ ob, op;
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(110, 120, 140));

            RoundBox(hdc, &scr, RGB(200, 208, 220), RGB(110, 120, 140));
            InflateRect(&scr, -3, -3);
            FillRect(hdc, &scr, nav);
            ob = SelectObject(hdc, moon);
            Ellipse(hdc, scr.left + S(8), scr.top + S(5), scr.left + S(20), scr.top + S(17));
            SelectObject(hdc, ob);
            op = SelectObject(hdc, pen);
            MoveToEx(hdc, cx, top + S(33), NULL); LineTo(hdc, cx, top + S(40));
            MoveToEx(hdc, cx - S(10), top + S(41), NULL); LineTo(hdc, cx + S(10), top + S(41));
            SelectObject(hdc, op);
            DeleteObject(pen);
            DeleteObject(nav);
            DeleteObject(moon);
            break;
        }
    }
}

static void
DrawTile(HDC hdc, PAGEDATA *pd, LINK *l, BOOL hot)
{
    int cx = (l->Rect.left + l->Rect.right) / 2;
    RECT t = l->Rect;

    if (hot)
    {
        HBRUSH b = CreateSolidBrush(RGB(226, 237, 250)), f = CreateSolidBrush(RGB(170, 200, 235));

        FillRect(hdc, &l->Rect, b);
        FrameRect(hdc, &l->Rect, f);
        DeleteObject(b);
        DeleteObject(f);
    }
    DrawTileIcon(hdc, pd, l->Tile, cx, l->Rect.top + S(6));
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, hot ? G->LinkFont : G->Font);
    SetTextColor(hdc, hot ? ColLinkHot : ColLink);
    t.top = l->Rect.top + S(56);
    t.bottom = t.top + S(18);
    DrawTextW(hdc, l->Text, -1, &t, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(110, 110, 110));
    t.top = t.bottom;
    t.bottom = t.top + S(18);
    DrawTextW(hdc, l->Value, -1, &t, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static HWND
RootOf(PAGEDATA *pd)
{
    HWND root = GetAncestor(pd->Hwnd, GA_ROOT);

    return root ? root : pd->Hwnd;
}

static void
DoAction(PAGEDATA *pd, int action)
{
    HWND hwnd = RootOf(pd);

    switch (action)
    {
        case ACT_HOME:
            Navigate(pd, LWNAV_CPHOME);
            break;
        case ACT_POINTERS:
            Launch(hwnd, L"rundll32.exe", L"shell32.dll,Control_RunDLL main.cpl,,1");
            break;
        case ACT_DISPLAY:
            Navigate(pd, LWPAGE_DISPLAY);
            break;
        case ACT_TASKBAR:
            Launch(hwnd, L"rundll32.exe", L"shell32.dll,Options_RunDLL 1");
            break;
        case ACT_EASE:
            Launch(hwnd, L"control.exe", L"access.cpl");
            break;
        case ACT_BACKGROUND:
            Navigate(pd, LWPAGE_BACKGROUND);
            break;
        case ACT_COLOR:
            Navigate(pd, LWPAGE_WINCOLOR);
            break;
        case ACT_SOUNDS:
            Launch(hwnd, L"rundll32.exe", L"shell32.dll,Control_RunDLL mmsys.cpl,,1");
            break;
        case ACT_RESOLUTION:
            Navigate(pd, LWPAGE_RESOLUTION);
            break;
        case ACT_PERSONALIZE:
            Navigate(pd, LWPAGE_MAIN);
            break;
        case ACT_ADVANCED:
            Launch(hwnd, L"rundll32.exe", L"shell32.dll,Control_RunDLL desk.cpl,,2");
            break;
    }
}

/* SCREEN SAVER DIALOG ******************************************************/

static INT_PTR CALLBACK
SaverHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_INITDIALOG:
        {
            RECT rc = { 6, 6, 316, 196 };
            HWND child;

            MapDialogRect(hwnd, &rc);
            child = CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_SAVER), hwnd, SaverProc, 0);
            if (child)
            {
                SetWindowLongPtrW(child, GWLP_ID, IDC_SAVER_CHILD);
                SetWindowPos(child, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);
                ShowWindow(GetDlgItem(child, IDC_SAVER_APPLY), SW_HIDE);
            }
            return TRUE;
        }
        case WM_COMMAND:
        {
            HWND child = GetDlgItem(hwnd, IDC_SAVER_CHILD);

            switch (LOWORD(wParam))
            {
                case IDOK:
                    if (child)
                        SendMessageW(child, WM_COMMAND, MAKEWPARAM(IDC_SAVER_APPLY, BN_CLICKED), 0);
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
                case IDC_SAVER_HOSTAPPLY:
                    if (child)
                        SendMessageW(child, WM_COMMAND, MAKEWPARAM(IDC_SAVER_APPLY, BN_CLICKED), 0);
                    return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

/* PERSONALIZATION (main) ***************************************************/

static void
CurrentWallpaperLabel(WCHAR *out, int cch, WCHAR *path, int pathCch)
{
    LWALL_STYLE style;

    path[0] = 0;
    if (LWallGetCurrent(path, pathCch, &style) && path[0])
        FileLabel(path, out, cch);
    else
        lstrcpynW(out, Str(IDS_CP_VALUE_SOLID), cch);
}

static void
SoundSchemeLabel(WCHAR *out, int cch)
{
    HKEY key;
    DWORD type, size = (cch - 1) * sizeof(WCHAR);

    lstrcpynW(out, L"-", cch);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"AppEvents\\Schemes", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        WCHAR v[96];

        v[0] = 0;
        size = sizeof(v) - sizeof(WCHAR);
        if (RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)v, &size) == ERROR_SUCCESS && type == REG_SZ && v[0])
        {
            v[size / sizeof(WCHAR)] = 0;
            if (!lstrcmpiW(v, L".Default"))
                lstrcpynW(out, Str(IDS_CP_VALUE_DEFAULT), cch);
            else if (!lstrcmpiW(v, L".None"))
                lstrcpynW(out, Str(IDS_CP_VALUE_NONE), cch);
            else
                lstrcpynW(out, v, cch);
        }
        RegCloseKey(key);
    }
}

static void
SaverLabel(WCHAR *out, int cch)
{
    WCHAR path[MAX_PATH];
    UINT timeout;
    BOOL active;

    lstrcpynW(out, Str(IDS_CP_VALUE_NONE), cch);
    if ((Caps() & LDISP_CAP_SCREENSAVER) && LSaverGet(path, ARRAYSIZE(path), &timeout, &active) && path[0] && active)
        FileLabel(path, out, cch);
}

static void
PersonalizeSetup(PAGEDATA *pd)
{
    LINK *l;
    WCHAR path[MAX_PATH];
    int gi, ti;
    TGITEM item;

    AddLink(pd, ACT_HOME, LK_TEXT, TRUE, IDS_CP_HOME);
    AddLink(pd, ACT_ICONS, LK_TEXT, FALSE, IDS_CP_CHANGE_ICONS);
    AddLink(pd, ACT_POINTERS, LK_TEXT, TRUE, IDS_CP_CHANGE_POINTERS);
    AddLink(pd, ACT_ACCOUNT, LK_TEXT, FALSE, IDS_CP_CHANGE_ACCOUNT);
    AddLink(pd, ACT_DISPLAY, LK_TEXT, TRUE, IDS_CP_LINK_DISPLAY);
    AddLink(pd, ACT_TASKBAR, LK_TEXT, TRUE, IDS_CP_LINK_TASKBAR);
    AddLink(pd, ACT_EASE, LK_TEXT, TRUE, IDS_CP_LINK_EASE);
    AddLink(pd, ACT_SAVETHEME, LK_TEXT, FALSE, IDS_CP_SAVE_THEME);
    AddLink(pd, ACT_MORETHEMES, LK_TEXT, FALSE, IDS_CP_MORE_THEMES);

    l = AddLink(pd, ACT_BACKGROUND, LK_TILE, TRUE, IDS_CP_TILE_BG);
    l->Tile = TILE_BG;
    CurrentWallpaperLabel(l->Value, ARRAYSIZE(l->Value), path, ARRAYSIZE(path));
    if (path[0])
    {
        HDC dc = GetDC(NULL);

        pd->BgThumb = TgMakeThumb(dc, path, S(52) - 6, S(44) - 6);
        ReleaseDC(NULL, dc);
    }
    l = AddLink(pd, ACT_COLOR, LK_TILE, TRUE, IDS_CP_TILE_COLOR);
    l->Tile = TILE_COLOR;
    lstrcpynW(l->Value, Str(IDS_CP_VALUE_CUSTOM), ARRAYSIZE(l->Value));
    l = AddLink(pd, ACT_SOUNDS, LK_TILE, TRUE, IDS_CP_TILE_SOUNDS);
    l->Tile = TILE_SOUNDS;
    SoundSchemeLabel(l->Value, ARRAYSIZE(l->Value));
    l = AddLink(pd, ACT_SAVER, LK_TILE, (Caps() & LDISP_CAP_SCREENSAVER) != 0, IDS_CP_TILE_SAVER);
    l->Tile = TILE_SAVER;
    SaverLabel(l->Value, ARRAYSIZE(l->Value));

    /* Theme gallery. The "Unsaved Theme" is the live combination; installed themes join through the theme model. */
    SendMessageW(pd->Grid, TGM_CLEAR, 0, 0);
    gi = (int)SendMessageW(pd->Grid, TGM_ADDGROUP, 0, (LPARAM)Str(IDS_CP_GROUP_MY));
    ZeroMemory(&item, sizeof(item));
    item.Group = gi;
    item.Text = Str(IDS_CP_THEME_UNSAVED);
    item.Image = path[0] ? path : NULL;
    item.Fill = GetSysColor(COLOR_DESKTOP);
    item.HasSwatch = TRUE;
    item.Swatch = GetSysColor(COLOR_ACTIVECAPTION);
    ti = (int)SendMessageW(pd->Grid, TGM_ADDITEM, 0, (LPARAM)&item);
    SendMessageW(pd->Grid, TGM_SETSEL, (WPARAM)ti, 0);
}

static void PageLayout(HWND hwnd, PAGEDATA *pd);

/* After a dialog changed a value (screen saver), rebuild the strip in place. */
static void
PersonalizeRefresh(PAGEDATA *pd)
{
    if (pd->BgThumb)
    {
        DeleteObject(pd->BgThumb);
        pd->BgThumb = NULL;
    }
    pd->LinkCount = 0;
    pd->Hot = -1;
    PersonalizeSetup(pd);
    PageLayout(pd->Hwnd, pd);
    InvalidateRect(pd->Hwnd, NULL, FALSE);
}

static void
PersonalizeLayout(PAGEDATA *pd, int w, int h)
{
    int pane = pd->PaneWidth = S(200), i, pad = S(14), tileW, x0, stripH = S(120), n = 0, tx;
    int yPane = S(14);
    int linksY;

    /* left pane: three tasks on top, See also at the bottom */
    for (i = 0; i < pd->LinkCount; i++)
    {
        LINK *l = &pd->Links[i];

        switch (l->Action)
        {
            case ACT_HOME: SetRect(&l->Rect, S(14), yPane, pane - S(8), yPane + S(36)); break;
            case ACT_ICONS: SetRect(&l->Rect, S(14), yPane + S(40), pane - S(8), yPane + S(58)); break;
            case ACT_POINTERS: SetRect(&l->Rect, S(14), yPane + S(62), pane - S(8), yPane + S(80)); break;
            case ACT_ACCOUNT: SetRect(&l->Rect, S(14), yPane + S(84), pane - S(8), yPane + S(102)); break;
            case ACT_DISPLAY: SetRect(&l->Rect, S(14), h - S(86), pane - S(8), h - S(68)); break;
            case ACT_TASKBAR: SetRect(&l->Rect, S(14), h - S(64), pane - S(8), h - S(46)); break;
            case ACT_EASE: SetRect(&l->Rect, S(14), h - S(42), pane - S(8), h - S(24)); break;
        }
    }
    x0 = pane + pad;
    linksY = h - stripH - S(26);
    MoveWindow(pd->Grid, x0, S(86), w - x0 - pad, linksY - S(86) - S(2), TRUE);
    /* Save theme / Get more themes sit under the gallery */
    tx = x0 + S(4);
    for (i = 0; i < pd->LinkCount; i++)
    {
        LINK *l = &pd->Links[i];

        if (l->Action == ACT_SAVETHEME)
            SetRect(&l->Rect, tx, linksY, tx + S(130), linksY + S(20));
        else if (l->Action == ACT_MORETHEMES)
            SetRect(&l->Rect, tx + S(140), linksY, tx + S(380), linksY + S(20));
    }
    /* bottom strip tiles */
    for (i = 0; i < pd->LinkCount; i++)
        n += pd->Links[i].Kind == LK_TILE;
    tileW = (w - pane - 2 * pad) / (n ? n : 1);
    if (tileW > S(190))
        tileW = S(190);
    {
        int total = tileW * n, start = pane + (w - pane - total) / 2, k = 0;

        for (i = 0; i < pd->LinkCount; i++)
        {
            LINK *l = &pd->Links[i];

            if (l->Kind != LK_TILE)
                continue;
            SetRect(&l->Rect, start + k * tileW + S(6), h - stripH + S(6), start + (k + 1) * tileW - S(6), h - S(6));
            k++;
        }
    }
}

static void
PersonalizePaint(HDC hdc, PAGEDATA *pd, int w, int h)
{
    RECT rc = { 0, 0, w, h }, pane, strip, t;
    int pad = S(14), x0 = pd->PaneWidth + pad, i;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));

    FillRect(hdc, &rc, white);
    DeleteObject(white);
    SetRect(&pane, 0, 0, pd->PaneWidth, h);
    FillGradient(hdc, &pane, RGB(237, 243, 251), RGB(214, 228, 245));
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(185, 205, 230));
        HGDIOBJ o = SelectObject(hdc, p);

        MoveToEx(hdc, pd->PaneWidth - 1, 0, NULL);
        LineTo(hdc, pd->PaneWidth - 1, h);
        SelectObject(hdc, o);
        DeleteObject(p);
    }
    SetBkMode(hdc, TRANSPARENT);
    /* "See also" caption */
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(90, 100, 120));
    SetRect(&t, S(14), h - S(110), pd->PaneWidth - S(8), h - S(90));
    DrawTextW(hdc, Str(IDS_CP_SEE_ALSO), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    /* heading + explanation */
    SelectObject(hdc, G->HeadFont);
    SetTextColor(hdc, ColHead);
    SetRect(&t, x0, S(14), w - pad, S(46));
    DrawTextW(hdc, Str(IDS_CP_PERS_HEAD), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(30, 30, 30));
    SetRect(&t, x0, S(50), w - pad, S(80));
    DrawTextW(hdc, Str(IDS_CP_PERS_DESC), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    /* gallery frame */
    {
        RECT g;
        HBRUSH f = CreateSolidBrush(RGB(130, 135, 143));

        GetWindowRect(pd->Grid, &g);
        MapWindowPoints(NULL, GetParent(pd->Grid), (POINT *)&g, 2);
        InflateRect(&g, 1, 1);
        FrameRect(hdc, &g, f);
        DeleteObject(f);
    }
    /* bottom strip */
    SetRect(&strip, pd->PaneWidth, h - S(120), w, h);
    FillGradient(hdc, &strip, RGB(238, 244, 252), RGB(221, 233, 248));
    for (i = 0; i < pd->LinkCount; i++)
    {
        LINK *l = &pd->Links[i];

        if (l->Kind == LK_TILE)
            DrawTile(hdc, pd, l, pd->Hot == i);
        else
            DrawTextLink(hdc, pd, l, pd->Hot == i);
    }
}

/* DESKTOP BACKGROUND *******************************************************/

static BOOL
IsPicture(const WCHAR *name)
{
    const WCHAR *dot = wcsrchr(name, L'.');

    return dot && (!lstrcmpiW(dot, L".bmp") || !lstrcmpiW(dot, L".jpg") || !lstrcmpiW(dot, L".jpeg") ||
                   !lstrcmpiW(dot, L".png"));
}

static int
CountPictures(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", dir)))
        return 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsPicture(fd.cFileName))
            n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void
AddPictures(HWND grid, int group, const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH], full[MAX_PATH], name[64];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    TGITEM item;

    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", dir)))
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsPicture(fd.cFileName) ||
            FAILED(StringCchPrintfW(full, ARRAYSIZE(full), L"%s\\%s", dir, fd.cFileName)))
            continue;
        FileLabel(fd.cFileName, name, ARRAYSIZE(name));
        ZeroMemory(&item, sizeof(item));
        item.Group = group;
        item.Text = name;
        item.Image = full;
        item.Fill = RGB(205, 210, 218);
        SendMessageW(grid, TGM_ADDITEM, 0, (LPARAM)&item);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void
ScanLocation(PAGEDATA *pd, int loc)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH], cur[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int g, i, total;
    LWALL_STYLE style;

    SendMessageW(pd->Grid, TGM_CLEAR, 0, 0);
    if (loc < 0 || loc >= pd->LocCount)
        return;
    if (CountPictures(pd->Locs[loc].Path))
    {
        g = (int)SendMessageW(pd->Grid, TGM_ADDGROUP, 0, (LPARAM)pd->Locs[loc].Name);
        if (g >= 0)
            AddPictures(pd->Grid, g, pd->Locs[loc].Path);
    }
    if (SUCCEEDED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", pd->Locs[loc].Path)))
    {
        h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
                    FAILED(StringCchPrintfW(sub, ARRAYSIZE(sub), L"%s\\%s", pd->Locs[loc].Path, fd.cFileName)) ||
                    !CountPictures(sub))
                    continue;
                g = (int)SendMessageW(pd->Grid, TGM_ADDGROUP, 0, (LPARAM)fd.cFileName);
                if (g >= 0)
                    AddPictures(pd->Grid, g, sub);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    total = (int)SendMessageW(pd->Grid, TGM_GETCOUNT, 0, 0);
    ShowWindow(pd->Empty, total ? SW_HIDE : SW_SHOW);
    ShowWindow(pd->Grid, total ? SW_SHOW : SW_HIDE);
    /* mark the picture that is the background now */
    cur[0] = 0;
    if (LWallGetCurrent(cur, ARRAYSIZE(cur), &style) && cur[0])
    {
        for (i = 0; i < total; i++)
        {
            WCHAR p[MAX_PATH];

            if (SendMessageW(pd->Grid, TGM_GETIMAGE, (WPARAM)i, (LPARAM)p) && SamePathCi(p, cur))
            {
                SendMessageW(pd->Grid, TGM_SETCHECK, (WPARAM)i, TRUE);
                SendMessageW(pd->Grid, TGM_SETSEL, (WPARAM)i, 0);
                break;
            }
        }
    }
}

static void
AddLocation(PAGEDATA *pd, const WCHAR *path, const WCHAR *name)
{
    int idx;

    if (pd->LocCount >= MAX_LOCS)
        return;
    lstrcpynW(pd->Locs[pd->LocCount].Path, path, MAX_PATH);
    lstrcpynW(pd->Locs[pd->LocCount].Name, name, ARRAYSIZE(pd->Locs[0].Name));
    idx = pd->LocCount++;
    SendMessageW(pd->Loc, CB_ADDSTRING, 0, (LPARAM)name);
    UNREFERENCED_PARAMETER(idx);
}

static HWND
Ctl(PAGEDATA *pd, HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(UINT_PTR)id,
                             hInst, NULL);

    if (h)
        SendMessageW(h, WM_SETFONT, (WPARAM)G->Font, FALSE);
    UNREFERENCED_PARAMETER(pd);
    return h;
}

static void
BackgroundCreate(HWND hwnd, PAGEDATA *pd)
{
    WCHAR sys[MAX_PATH], dir[MAX_PATH], cur[MAX_PATH];
    LWALL_STYLE style = LWALL_FILL;
    int i, sel = 0;

    pd->Loc = Ctl(pd, hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_LOC);
    pd->Browse = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_BROWSE), BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE);
    pd->SelAll = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_SELALL), BS_PUSHBUTTON | WS_DISABLED | WS_TABSTOP, ID_SELALL);
    pd->ClearAll = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_CLEARALL), BS_PUSHBUTTON | WS_DISABLED | WS_TABSTOP, ID_CLEARALL);
    pd->Grid = CreateWindowExW(WS_EX_CLIENTEDGE, TG_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                               TGS_CHECKBOXES, 0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)ID_GRID, hInst, NULL);
    pd->Empty = Ctl(pd, hwnd, L"STATIC", Str(IDS_CP_BG_EMPTY), SS_CENTER, ID_EMPTY);
    ShowWindow(pd->Empty, SW_HIDE);
    pd->Pos = Ctl(pd, hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_POS);
    pd->Interval = Ctl(pd, hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_DISABLED, ID_INTERVAL);
    pd->Shuffle = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_SHUFFLE), BS_AUTOCHECKBOX | WS_DISABLED, ID_SHUFFLE);
    pd->Note = Ctl(pd, hwnd, L"STATIC", Str(IDS_CP_BG_SLIDE_NA), SS_LEFT, ID_NOTE);
    pd->Save = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_SAVE), BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE);
    pd->Cancel = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_CANCEL), BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL);
    if (pd->Grid)
        SendMessageW(pd->Grid, WM_SETFONT, (WPARAM)G->Font, FALSE);

    {
        static const UINT styleIds[LWALL_STYLE_COUNT] =
            { IDS_STYLE_FILL, IDS_STYLE_FIT, IDS_STYLE_STRETCH, IDS_STYLE_TILE, IDS_STYLE_CENTER };

        for (i = 0; i < LWALL_STYLE_COUNT; i++)
            SendMessageW(pd->Pos, CB_ADDSTRING, 0, (LPARAM)Str(styleIds[i]));
        cur[0] = 0;
        if (LWallGetCurrent(cur, ARRAYSIZE(cur), &style))
            SendMessageW(pd->Pos, CB_SETCURSEL, (WPARAM)style, 0);
        else
            SendMessageW(pd->Pos, CB_SETCURSEL, LWALL_FILL, 0);
    }
    SendMessageW(pd->Interval, CB_ADDSTRING, 0, (LPARAM)Str(IDS_CP_INTERVAL_30));
    SendMessageW(pd->Interval, CB_SETCURSEL, 0, 0);

    if (GetWindowsDirectoryW(sys, ARRAYSIZE(sys)) &&
        SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\Web\\Wallpaper", sys)))
        AddLocation(pd, dir, Str(IDS_CP_LOC_DESKTOP));
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, dir)) && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES)
        AddLocation(pd, dir, Str(IDS_CP_LOC_PICTURES));
    /* the current picture may live outside both: show its folder as a third place */
    if (cur[0])
    {
        BOOL known = FALSE;
        WCHAR folder[MAX_PATH], *slash;

        lstrcpynW(folder, cur, ARRAYSIZE(folder));
        slash = wcsrchr(folder, L'\\');
        if (slash)
            *slash = 0;
        for (i = 0; i < pd->LocCount; i++)
        {
            WCHAR *rootEnd = pd->Locs[i].Path;
            size_t n = wcslen(rootEnd);

            if (!_wcsnicmp(folder, rootEnd, n))
                known = TRUE;
        }
        if (!known && slash)
        {
            WCHAR name[64];

            lstrcpynW(name, slash > folder && wcsrchr(folder, L'\\') ? wcsrchr(folder, L'\\') + 1 : folder, ARRAYSIZE(name));
            AddLocation(pd, folder, name[0] ? name : folder);
            sel = pd->LocCount - 1;
        }
    }
    SendMessageW(pd->Loc, CB_SETCURSEL, (WPARAM)(pd->LocCount ? sel : -1), 0);
    ScanLocation(pd, pd->LocCount ? sel : -1);
}

static void
BackgroundLayout(PAGEDATA *pd, int w, int h)
{
    int pad = S(18), y = S(78), bh = S(24), barH = S(48), gridTop, gridBot, posY;
    int locW = S(210), x = pad;

    MoveWindow(pd->Loc, x + S(0), y + S(22), locW, S(200), TRUE);
    x += locW + S(8);
    MoveWindow(pd->Browse, x, y + S(21), S(90), bh, TRUE);
    x += S(96);
    MoveWindow(pd->SelAll, x, y + S(21), S(100), bh, TRUE);
    x += S(106);
    MoveWindow(pd->ClearAll, x, y + S(21), S(100), bh, TRUE);
    gridTop = y + S(56);
    posY = h - barH - S(86);
    gridBot = posY - S(14);
    MoveWindow(pd->Grid, pad, gridTop, w - 2 * pad, gridBot - gridTop, TRUE);
    MoveWindow(pd->Empty, pad + S(20), gridTop + S(60), w - 2 * pad - S(40), S(24), TRUE);
    MoveWindow(pd->Pos, pad + S(76), posY + S(22), S(110), S(200), TRUE);
    MoveWindow(pd->Interval, pad + S(250), posY + S(22), S(130), S(200), TRUE);
    MoveWindow(pd->Shuffle, pad + S(392), posY + S(23), S(150), S(22), TRUE);
    MoveWindow(pd->Note, pad + S(250), posY + S(52), w - pad - S(250) - pad, S(20), TRUE);
    MoveWindow(pd->Save, w - 2 * pad - S(180), h - barH + S(12), S(100), bh + S(2), TRUE);
    MoveWindow(pd->Cancel, w - pad - S(76), h - barH + S(12), S(76), bh + S(2), TRUE);
}

static void
PosPreview(HDC hdc, PAGEDATA *pd, const RECT *r)
{
    int style = (int)SendMessageW(pd->Pos, CB_GETCURSEL, 0, 0), x, y;
    RECT in = *r;
    HBRUSH monitor = CreateSolidBrush(RGB(34, 40, 52)), pic = CreateSolidBrush(RGB(224, 138, 64));
    int sel = (int)SendMessageW(pd->Grid, TGM_GETSEL, 0, 0);
    WCHAR path[MAX_PATH];
    HBITMAP bmp = NULL;

    FillRect(hdc, r, monitor);
    InflateRect(&in, -3, -3);
    if (sel >= 0 && SendMessageW(pd->Grid, TGM_GETIMAGE, (WPARAM)sel, (LPARAM)path))
        bmp = TgMakeThumb(hdc, path, in.right - in.left, in.bottom - in.top);
    switch (style)
    {
        case LWALL_FIT:
            InflateRect(&in, -(in.right - in.left) / 6, 0);
            break;
        case LWALL_CENTER:
            InflateRect(&in, -(in.right - in.left) / 4, -(in.bottom - in.top) / 4);
            break;
        default:
            break;
    }
    if (bmp)
    {
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, bmp);

        if (style == LWALL_TILE)
        {
            int tw = (r->right - r->left) / 3, th = (r->bottom - r->top) / 3;

            for (y = r->top + 3; y < r->bottom - 3; y += th)
                for (x = r->left + 3; x < r->right - 3; x += tw)
                    StretchBlt(hdc, x, y, min(tw - 1, r->right - 3 - x), min(th - 1, r->bottom - 3 - y), mem, 0, 0,
                               tw, th, SRCCOPY);
        }
        else
        {
            StretchBlt(hdc, in.left, in.top, in.right - in.left, in.bottom - in.top, mem, 0, 0,
                       (r->right - r->left) - 6, (r->bottom - r->top) - 6, SRCCOPY);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    else
    {
        FillRect(hdc, &in, pic);
    }
    DeleteObject(monitor);
    DeleteObject(pic);
}

static void
BackgroundPaint(HDC hdc, PAGEDATA *pd, int w, int h)
{
    RECT rc = { 0, 0, w, h }, bar, t;
    int pad = S(18), barH = S(48), posY = h - barH - S(86);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), grey = CreateSolidBrush(RGB(240, 240, 240));
    HBRUSH line = CreateSolidBrush(RGB(223, 223, 223));

    FillRect(hdc, &rc, white);
    SetRect(&bar, 0, h - barH, w, h);
    FillRect(hdc, &bar, grey);
    SetRect(&t, 0, h - barH, w, h - barH + 1);
    FillRect(hdc, &t, line);
    DeleteObject(white);
    DeleteObject(grey);
    DeleteObject(line);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, G->HeadFont);
    SetTextColor(hdc, ColHead);
    SetRect(&t, pad, S(14), w - pad, S(46));
    DrawTextW(hdc, Str(IDS_CP_BG_HEAD), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(30, 30, 30));
    SetRect(&t, pad, S(50), w - pad, S(72));
    DrawTextW(hdc, Str(IDS_CP_BG_DESC), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetRect(&t, pad, S(78), pad + S(200), S(96));
    DrawTextW(hdc, Str(IDS_CP_BG_LOCATION), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetRect(&t, pad, posY, pad + S(200), posY + S(18));
    DrawTextW(hdc, Str(IDS_CP_BG_POSITION), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetRect(&t, pad + S(250), posY, pad + S(500), posY + S(18));
    DrawTextW(hdc, Str(IDS_CP_BG_INTERVAL), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    {
        RECT pv = { pad, posY + S(20), pad + S(64), posY + S(20) + S(46) };

        PosPreview(hdc, pd, &pv);
    }
}

static void
BackgroundExclusive(PAGEDATA *pd)
{
    int sel = (int)SendMessageW(pd->Grid, TGM_GETSEL, 0, 0), i, n = (int)SendMessageW(pd->Grid, TGM_GETCOUNT, 0, 0);

    /* one picture is the desktop background; several would need a slide show, which is not available */
    for (i = 0; i < n; i++)
        SendMessageW(pd->Grid, TGM_SETCHECK, (WPARAM)i, i == sel);
    InvalidateRect(GetParent(pd->Grid), NULL, FALSE);
}

static void
BackgroundSave(HWND hwnd, PAGEDATA *pd)
{
    int n = (int)SendMessageW(pd->Grid, TGM_GETCOUNT, 0, 0), i, pick = -1;
    int style = (int)SendMessageW(pd->Pos, CB_GETCURSEL, 0, 0);
    WCHAR path[MAX_PATH];

    for (i = 0; i < n && pick < 0; i++)
    {
        if (SendMessageW(pd->Grid, TGM_GETCHECK, (WPARAM)i, 0))
            pick = i;
    }
    if (pick < 0)
        pick = (int)SendMessageW(pd->Grid, TGM_GETSEL, 0, 0);
    if (pick >= 0 && SendMessageW(pd->Grid, TGM_GETIMAGE, (WPARAM)pick, (LPARAM)path))
    {
        if (!LWallSet(path, (LWALL_STYLE)(style < 0 ? LWALL_FILL : style)))
        {
            Msg(hwnd, IDS_WALL_FAILED, MB_OK | MB_ICONERROR);
            return;
        }
    }
    Navigate(pd, LWPAGE_MAIN);
}

static void
BackgroundBrowse(HWND hwnd, PAGEDATA *pd)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    WCHAR path[MAX_PATH];

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = Str(IDS_CP_BROWSE_TITLE);
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return;
    if (SHGetPathFromIDListW(pidl, path) && pd->LocCount < MAX_LOCS)
    {
        WCHAR name[64];

        lstrcpynW(name, wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path, ARRAYSIZE(name));
        AddLocation(pd, path, name[0] ? name : path);
        SendMessageW(pd->Loc, CB_SETCURSEL, (WPARAM)(pd->LocCount - 1), 0);
        ScanLocation(pd, pd->LocCount - 1);
    }
    CoTaskMemFree(pidl);
}

/* WINDOW COLOR / DISPLAY / RESOLUTION *************************************/

static const COLORREF Palette[16] =
{
    RGB(0, 102, 204), RGB(0, 120, 215), RGB(0, 153, 188), RGB(0, 153, 120),
    RGB(16, 124, 16), RGB(107, 142, 35), RGB(255, 140, 0), RGB(209, 52, 56),
    RGB(194, 57, 179), RGB(136, 23, 152), RGB(91, 83, 180), RGB(72, 104, 145),
    RGB(100, 100, 100), RGB(68, 68, 68), RGB(30, 30, 30), RGB(185, 185, 185)
};

static void
ColorSetup(HWND hwnd, PAGEDATA *pd)
{
    AddLink(pd, ACT_ADVANCED, LK_TEXT, TRUE, IDS_CP_COLOR_ADV);
    pd->ColorSel = LWinColorGet();
    pd->Save = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_BG_SAVE), BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE);
    pd->Cancel = Ctl(pd, hwnd, L"BUTTON", Str(IDS_CP_CANCEL), BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL);
}

static void
ColorLayout(PAGEDATA *pd, int w, int h)
{
    int pad = S(18), barH = S(48), i, sz = S(44), gap = S(10);

    for (i = 0; i < 16; i++)
    {
        int col = i % 8, row = i / 8;

        SetRect(&pd->Swatch[i], pad + col * (sz + gap), S(150) + row * (sz + gap),
                pad + col * (sz + gap) + sz, S(150) + row * (sz + gap) + sz);
    }
    SetRect(&pd->Links[0].Rect, pad, S(150) + 2 * (sz + gap) + S(14), pad + S(300), S(150) + 2 * (sz + gap) + S(36));
    MoveWindow(pd->Save, w - 2 * pad - S(180), h - barH + S(12), S(100), S(26), TRUE);
    MoveWindow(pd->Cancel, w - pad - S(76), h - barH + S(12), S(76), S(26), TRUE);
}

static void
ColorSave(HWND hwnd, PAGEDATA *pd)
{
    if (pd->ColorSel != LWinColorGet() && !LWinColorSet(pd->ColorSel))
    {
        Msg(hwnd, IDS_CP_COLOR_FAILED, MB_OK | MB_ICONERROR);
        return;
    }
    Navigate(pd, LWPAGE_MAIN);
}

static void
ColorPaint(HDC hdc, PAGEDATA *pd, int w, int h)
{
    RECT rc = { 0, 0, w, h }, t, bar, sw = { 0, 0, 0, 0 };
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), grey = CreateSolidBrush(RGB(240, 240, 240));
    int pad = S(18), barH = S(48), i;

    FillRect(hdc, &rc, white);
    DeleteObject(white);
    SetRect(&bar, 0, h - barH, w, h);
    FillRect(hdc, &bar, grey);
    DeleteObject(grey);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, G->HeadFont);
    SetTextColor(hdc, ColHead);
    SetRect(&t, pad, S(14), w - pad, S(46));
    DrawTextW(hdc, Str(IDS_CP_COLOR_HEAD), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(30, 30, 30));
    SetRect(&t, pad, S(50), w - pad, S(76));
    DrawTextW(hdc, Str(IDS_CP_COLOR_DESC), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    /* preview of the chosen colour as a title bar */
    SetRect(&sw, pad, S(84), pad + S(260), S(84) + S(48));
    {
        HBRUSH frame = CreateSolidBrush(RGB(120, 130, 150)), cap = CreateSolidBrush(pd->ColorSel);
        RECT c = sw;

        FillRect(hdc, &sw, white = CreateSolidBrush(RGB(250, 250, 250)));
        DeleteObject(white);
        c.bottom = c.top + S(22);
        FillRect(hdc, &c, cap);
        FrameRect(hdc, &sw, frame);
        DeleteObject(frame);
        DeleteObject(cap);
        SetTextColor(hdc, (GetRValue(pd->ColorSel) * 299 + GetGValue(pd->ColorSel) * 587 + GetBValue(pd->ColorSel) * 114) > 128000
                          ? RGB(0, 0, 0) : RGB(255, 255, 255));
        c.left += S(8);
        DrawTextW(hdc, Str(IDS_CP_COLOR_CURRENT), -1, &c, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (i = 0; i < 16; i++)
    {
        HBRUSH fill = CreateSolidBrush(Palette[i]);
        HBRUSH edge = CreateSolidBrush(pd->ColorSel == Palette[i] ? RGB(0, 70, 170) : RGB(150, 155, 165));
        RECT r = pd->Swatch[i];

        FillRect(hdc, &r, fill);
        FrameRect(hdc, &r, edge);
        if (pd->ColorSel == Palette[i])
        {
            InflateRect(&r, 2, 2);
            FrameRect(hdc, &r, edge);
            InflateRect(&r, 1, 1);
            FrameRect(hdc, &r, edge);
        }
        DeleteObject(fill);
        DeleteObject(edge);
    }
    DrawTextLink(hdc, pd, &pd->Links[0], pd->Hot == 0);
}

static void
DisplaySetup(PAGEDATA *pd)
{
    AddLink(pd, ACT_HOME, LK_TEXT, TRUE, IDS_CP_HOME);
    AddLink(pd, ACT_RESOLUTION, LK_TEXT, TRUE, IDS_CP_LINK_RESOLUTION);
    AddLink(pd, ACT_RESOLUTION, LK_TEXT, TRUE, IDS_CP_LINK_DISPSET);
    AddLink(pd, ACT_PERSONALIZE, LK_TEXT, TRUE, IDS_CPL_PERSONALIZE_NAME);
}

static void
DisplayLayout(PAGEDATA *pd, int w, int h)
{
    int pane = pd->PaneWidth = S(200), y = S(14);

    SetRect(&pd->Links[0].Rect, S(14), y, pane - S(8), y + S(36));
    SetRect(&pd->Links[1].Rect, S(14), y + S(40), pane - S(8), y + S(58));
    SetRect(&pd->Links[2].Rect, S(14), y + S(62), pane - S(8), y + S(80));
    SetRect(&pd->Links[3].Rect, S(14), h - S(42), pane - S(8), h - S(24));
    UNREFERENCED_PARAMETER(w);
}

static void
DisplayPaint(HDC hdc, PAGEDATA *pd, int w, int h)
{
    RECT rc = { 0, 0, w, h }, pane, t;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    int x0 = pd->PaneWidth + S(14), i;
    WCHAR text[160];

    FillRect(hdc, &rc, white);
    DeleteObject(white);
    SetRect(&pane, 0, 0, pd->PaneWidth, h);
    FillGradient(hdc, &pane, RGB(237, 243, 251), RGB(214, 228, 245));
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(90, 100, 120));
    SetRect(&t, S(14), h - S(66), pd->PaneWidth - S(8), h - S(46));
    DrawTextW(hdc, Str(IDS_CP_SEE_ALSO), -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, G->HeadFont);
    SetTextColor(hdc, ColHead);
    SetRect(&t, x0, S(14), w - S(14), S(46));
    DrawTextW(hdc, Str(IDS_CP_DISP_HEAD), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, G->Font);
    SetTextColor(hdc, RGB(30, 30, 30));
    SetRect(&t, x0, S(50), w - S(14), S(76));
    DrawTextW(hdc, Str(IDS_CP_DISP_DESC), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    StringCchPrintfW(text, ARRAYSIZE(text), Str(IDS_CP_DISP_SCALE), LDispGetSystemDpi() * 100 / 96);
    SetRect(&t, x0, S(100), w - S(14), S(122));
    DrawTextW(hdc, text, -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, ColDisabled);
    SetRect(&t, x0, S(126), w - S(14), S(170));
    DrawTextW(hdc, Str(IDS_CP_DISP_SCALE_NA), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    for (i = 0; i < pd->LinkCount; i++)
        DrawTextLink(hdc, pd, &pd->Links[i], pd->Hot == i);
}

static void
ResolutionCreate(HWND hwnd, PAGEDATA *pd)
{
    pd->Child = CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_RESPAGE), hwnd, DisplayProc, 0);
    if (pd->Child)
        ShowWindow(pd->Child, SW_SHOW);
}

static void
ResolutionPaint(HDC hdc, PAGEDATA *pd, int w, int h)
{
    RECT rc = { 0, 0, w, h }, t;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));

    FillRect(hdc, &rc, white);
    DeleteObject(white);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, G->HeadFont);
    SetTextColor(hdc, ColHead);
    SetRect(&t, S(18), S(14), w - S(18), S(46));
    DrawTextW(hdc, Str(IDS_CP_RES_HEAD), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    UNREFERENCED_PARAMETER(pd);
    UNREFERENCED_PARAMETER(h);
}

/* PAGE WINDOW **************************************************************/

static void
PageLayout(HWND hwnd, PAGEDATA *pd)
{
    RECT rc;
    int w, h;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    switch (pd->Id)
    {
        case PG_PERSONALIZE: PersonalizeLayout(pd, w, h); break;
        case PG_BACKGROUND: BackgroundLayout(pd, w, h); break;
        case PG_COLOR: ColorLayout(pd, w, h); break;
        case PG_DISPLAY: DisplayLayout(pd, w, h); break;
        case PG_RESOLUTION:
            if (pd->Child)
                MoveWindow(pd->Child, S(18), S(54), w - S(36), h - S(70), TRUE);
            break;
    }
}

static int
HitLink(PAGEDATA *pd, int x, int y)
{
    int i;
    POINT pt = { x, y };

    for (i = 0; i < pd->LinkCount; i++)
    {
        if (PtInRect(&pd->Links[i].Rect, pt))
            return i;
    }
    return -1;
}

static LRESULT CALLBACK
PageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAGEDATA *pd = (PAGEDATA *)GetWindowLongPtrW(hwnd, 0);

    switch (msg)
    {
        case WM_NCCREATE:
        {
            CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;

            pd = (PAGEDATA *)LocalAlloc(LPTR, sizeof(*pd));
            if (!pd)
                return FALSE;
            {
                PAGECREATE *pc = (PAGECREATE *)cs->lpCreateParams;

                pd->Shell = G;
                pd->Hwnd = hwnd;
                pd->Id = (int)pc->Id;
                if (pc->Host)
                    pd->Host = *pc->Host;
            }
            pd->Hot = -1;
            SetWindowLongPtrW(hwnd, 0, (LONG_PTR)pd);
            return TRUE;
        }
        case WM_CREATE:
            switch (pd->Id)
            {
                case PG_PERSONALIZE:
                    pd->Grid = CreateWindowExW(0, TG_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                               TGS_LABELS | TGS_SWATCH, 0, 0, 10, 10, hwnd,
                                               (HMENU)(UINT_PTR)ID_GRID, hInst, NULL);
                    if (pd->Grid)
                    {
                        SendMessageW(pd->Grid, WM_SETFONT, (WPARAM)G->Font, FALSE);
                        SendMessageW(pd->Grid, TGM_SETCELL, (WPARAM)S(112), (LPARAM)S(70));
                    }
                    PersonalizeSetup(pd);
                    break;
                case PG_BACKGROUND: BackgroundCreate(hwnd, pd); break;
                case PG_COLOR: ColorSetup(hwnd, pd); break;
                case PG_DISPLAY: DisplaySetup(pd); break;
                case PG_RESOLUTION: ResolutionCreate(hwnd, pd); break;
            }
            return 0;
        case WM_SIZE:
            if (pd)
            {
                PageLayout(hwnd, pd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_NAVIGATE:
            if (pd && pd->Host.Navigate)
                pd->Host.Navigate(pd->Host.ctx, (UINT)wParam);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (pd)
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps), mem;
                RECT rc;
                HBITMAP bmp;
                HGDIOBJ ob, of;

                GetClientRect(hwnd, &rc);
                mem = CreateCompatibleDC(hdc);
                bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                ob = SelectObject(mem, bmp);
                of = SelectObject(mem, G->Font);
                switch (pd->Id)
                {
                    case PG_PERSONALIZE: PersonalizePaint(mem, pd, rc.right, rc.bottom); break;
                    case PG_BACKGROUND: BackgroundPaint(mem, pd, rc.right, rc.bottom); break;
                    case PG_COLOR: ColorPaint(mem, pd, rc.right, rc.bottom); break;
                    case PG_DISPLAY: DisplayPaint(mem, pd, rc.right, rc.bottom); break;
                    case PG_RESOLUTION: ResolutionPaint(mem, pd, rc.right, rc.bottom); break;
                }
                BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, of);
                SelectObject(mem, ob);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd, &ps);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (pd && pd->Id == PG_BACKGROUND)
            {
                SetBkMode((HDC)wParam, TRANSPARENT);
                SetTextColor((HDC)wParam, (HWND)lParam == pd->Note ? RGB(130, 130, 130) : RGB(30, 30, 30));
                return (LRESULT)GetStockObject(WHITE_BRUSH);
            }
            break;
        case WM_MOUSEMOVE:
            if (pd)
            {
                int hot = HitLink(pd, (short)LOWORD(lParam), (short)HIWORD(lParam));

                if (!G->Tracking)
                {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

                    G->Tracking = TrackMouseEvent(&tme);
                }
                if (hot >= 0 && !pd->Links[hot].Enabled)
                    hot = -1;
                if (hot != pd->Hot)
                {
                    pd->Hot = hot;
                    SetCursor(LoadCursorW(NULL, (LPCWSTR)(hot >= 0 ? IDC_HAND : IDC_ARROW)));
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            G->Tracking = FALSE;
            if (pd && pd->Hot != -1)
            {
                pd->Hot = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (pd && pd->Hot >= 0 && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            if (pd && pd->Id == PG_COLOR)
            {
                POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
                int k;

                for (k = 0; k < 16; k++)
                {
                    if (PtInRect(&pd->Swatch[k], pt))
                    {
                        pd->ColorSel = Palette[k];
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    }
                }
            }
            if (pd)
            {
                int i = HitLink(pd, (short)LOWORD(lParam), (short)HIWORD(lParam));

                if (i >= 0 && pd->Links[i].Enabled)
                {
                    int action = pd->Links[i].Action;

                    if (action == ACT_SAVER)
                    {
                        DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SAVER_DLG), RootOf(pd), SaverHostProc, 0);
                        PersonalizeRefresh(pd);   /* new strip values */
                    }
                    else
                    {
                        DoAction(pd, action);
                    }
                }
            }
            return 0;
        case WM_COMMAND:
            if (!pd)
                break;
            switch (LOWORD(wParam))
            {
                case ID_LOC:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                        ScanLocation(pd, (int)SendMessageW(pd->Loc, CB_GETCURSEL, 0, 0));
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                case ID_BROWSE: BackgroundBrowse(hwnd, pd); InvalidateRect(hwnd, NULL, FALSE); return 0;
                case ID_POS: InvalidateRect(hwnd, NULL, FALSE); return 0;
                case IDC_DISP_DONE: Navigate(pd, LWPAGE_DISPLAY); return 0;
                case ID_SAVE:
                    if (pd->Id == PG_COLOR)
                        ColorSave(hwnd, pd);
                    else
                        BackgroundSave(hwnd, pd);
                    return 0;
                case ID_CANCEL: Navigate(pd, LWPAGE_MAIN); return 0;
                case ID_GRID:
                    if (pd->Id == PG_BACKGROUND)
                    {
                        if (HIWORD(wParam) == TGN_SEL || HIWORD(wParam) == TGN_CHECK)
                        {
                            if (HIWORD(wParam) == TGN_SEL)
                                SendMessageW(pd->Grid, TGM_SETCHECK,
                                             (WPARAM)SendMessageW(pd->Grid, TGM_GETSEL, 0, 0), TRUE);
                            BackgroundExclusive(pd);
                        }
                        if (HIWORD(wParam) == TGN_ACTIVATE)
                            BackgroundSave(hwnd, pd);
                    }
                    return 0;
            }
            break;
        case WM_NCDESTROY:
            if (pd)
            {
                if (pd->BgThumb)
                    DeleteObject(pd->BgThumb);
                LocalFree(pd);
                SetWindowLongPtrW(hwnd, 0, 0);
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* COMPONENT ABI ***********************************************************/

typedef struct COMPONENT
{
    HWND Hwnd;
} COMPONENT;

/* The host leaves the message handler first: it may destroy this page from inside Navigate. */
static void
Navigate(PAGEDATA *pd, UINT page)
{
    PostMessageW(pd->Hwnd, WM_NAVIGATE, (WPARAM)page, 0);
}

static HFONT
MakeFont(const LOGFONTW *base, int dpi, int pointDelta, BOOL underline)
{
    LOGFONTW lf = *base;

    if (pointDelta)
        lf.lfHeight = lf.lfHeight < 0 ? lf.lfHeight - MulDiv(pointDelta, dpi, 72) : lf.lfHeight + MulDiv(pointDelta, dpi, 72);
    lf.lfUnderline = underline;
    return CreateFontIndirectW(&lf);
}

static BOOL
SharedAcquire(void)
{
    NONCLIENTMETRICSW ncm;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    struct GdiplusStartupInput gdipStartup;
    WNDCLASSEXW wc;

    if (G)
    {
        G->Refs++;
        return TRUE;
    }
    G = (SHELLDATA *)LocalAlloc(LPTR, sizeof(*G));
    if (!G)
        return FALSE;
    G->Refs = 1;
    G->Dpi = (int)LDispGetSystemDpi();
    if (G->Dpi < 96)
        G->Dpi = 96;
    CapsReset();
    InitCommonControlsEx(&icc);
    CoInitialize(NULL);
    gdipStartup.GdiplusVersion = 1;
    gdipStartup.DebugEventCallback = NULL;
    gdipStartup.SuppressBackgroundThread = FALSE;
    gdipStartup.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&G->GdipToken, &gdipStartup, NULL);
    TgRegister(hInst);
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        ZeroMemory(&ncm.lfMessageFont, sizeof(ncm.lfMessageFont));
    G->Font = MakeFont(&ncm.lfMessageFont, G->Dpi, 0, FALSE);
    G->LinkFont = MakeFont(&ncm.lfMessageFont, G->Dpi, 0, TRUE);
    G->HeadFont = MakeFont(&ncm.lfMessageFont, G->Dpi, 5, FALSE);
    if (!GetClassInfoExW(hInst, PAGE_CLASS, &wc))
    {
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PageProc;
        wc.cbWndExtra = sizeof(LONG_PTR);
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = PAGE_CLASS;
        RegisterClassExW(&wc);
    }
    return TRUE;
}

static void
SharedRelease(void)
{
    if (!G || --G->Refs > 0)
        return;
    UnregisterClassW(PAGE_CLASS, hInst);
    DeleteObject(G->Font);
    DeleteObject(G->LinkFont);
    DeleteObject(G->HeadFont);
    /* GDI+ stays started: shutting it down between two pages made the next page's thumbnails fail to load */
    CoUninitialize();
    LocalFree(G);
    G = NULL;
}

HWND WINAPI
LeewayPageCreate(UINT pageId, HWND hwndParent, const RECT *prc, const LWPAGE_HOST *host, void **ppPage)
{
    COMPONENT *c;
    PAGECREATE pc;

    if (ppPage)
        *ppPage = NULL;
    if (pageId >= PG_COUNT || !hwndParent || !prc || !ppPage || !SharedAcquire())
        return NULL;
    c = (COMPONENT *)LocalAlloc(LPTR, sizeof(*c));
    if (!c)
    {
        SharedRelease();
        return NULL;
    }
    pc.Id = pageId;
    pc.Host = host;
    c->Hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                              hwndParent, NULL, hInst, &pc);
    if (!c->Hwnd)
    {
        LocalFree(c);
        SharedRelease();
        return NULL;
    }
    *ppPage = c;
    return c->Hwnd;
}

void WINAPI
LeewayPageResize(void *page, const RECT *prc)
{
    COMPONENT *c = (COMPONENT *)page;

    if (c && c->Hwnd && prc)
        MoveWindow(c->Hwnd, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, TRUE);
}

BOOL WINAPI
LeewayPageTranslateKey(void *page, MSG *msg)
{
    COMPONENT *c = (COMPONENT *)page;

    return c && c->Hwnd && msg && IsDialogMessageW(c->Hwnd, msg);
}

void WINAPI
LeewayPageDestroy(void *page)
{
    COMPONENT *c = (COMPONENT *)page;

    if (!c)
        return;
    if (c->Hwnd && IsWindow(c->Hwnd))
        DestroyWindow(c->Hwnd);
    LocalFree(c);
    SharedRelease();
}
