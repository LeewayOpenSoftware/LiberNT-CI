/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Grouped thumbnail grid control (theme gallery, wallpaper picker)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <commctrl.h>
#include <shlobj.h>
#include <strsafe.h>
#include <gdiplus.h>
#include "thumbgrid.h"

#define TG_MAX_ITEMS  600
#define TG_MAX_GROUPS 16

typedef struct TGENTRY
{
    int Group;
    WCHAR Text[64];
    WCHAR Image[MAX_PATH];
    COLORREF Fill;
    BOOL HasSwatch;
    COLORREF Swatch;
    BOOL Checked;
    LPARAM Data;
    HBITMAP Thumb;
    BOOL ThumbTried;
} TGENTRY;

typedef struct TGGROUP
{
    WCHAR Name[64];
    BOOL Collapsed;
} TGGROUP;

typedef struct TGDATA
{
    TGENTRY *Items;
    int ItemCount;
    TGGROUP Groups[TG_MAX_GROUPS];
    int GroupCount;
    int Selected;
    int Hot;
    int Scroll;
    int ThumbW, ThumbH;
    HFONT Font;
    BOOL OwnFont;
    BOOL Tracking;
} TGDATA;

#define HEADER_H   26
#define PAD        12

static int
Dpi(HWND hwnd, int v)
{
    HDC hdc = GetDC(hwnd);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;

    if (hdc)
        ReleaseDC(hwnd, hdc);
    return MulDiv(v, dpi ? dpi : 96, 96);
}

static void
CellSize(HWND hwnd, TGDATA *td, int *cw, int *ch)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);

    *cw = td->ThumbW + 24;
    *ch = td->ThumbH + ((style & TGS_LABELS) ? 36 : 22);
}

/* Cell rectangle in content coordinates (no scroll); -1 total height out. */
typedef struct TGLAYOUT
{
    RECT Cell[TG_MAX_ITEMS];
    RECT Header[TG_MAX_GROUPS];
    int Total;
} TGLAYOUT;

static void
Layout(HWND hwnd, TGDATA *td, TGLAYOUT *lay)
{
    RECT rc;
    int cw, ch, g, i, y = PAD / 2, cols, col, avail;

    GetClientRect(hwnd, &rc);
    CellSize(hwnd, td, &cw, &ch);
    avail = rc.right - 2 * PAD - GetSystemMetrics(SM_CXVSCROLL);
    cols = avail / cw;
    if (cols < 1)
        cols = 1;
    for (g = 0; g < td->GroupCount; g++)
    {
        int n = 0;

        for (i = 0; i < td->ItemCount; i++)
            n += td->Items[i].Group == g;
        SetRect(&lay->Header[g], PAD / 2, y, rc.right - PAD / 2 - GetSystemMetrics(SM_CXVSCROLL), y + HEADER_H);
        y += HEADER_H + 4;
        col = 0;
        if (!td->Groups[g].Collapsed)
        {
            for (i = 0; i < td->ItemCount; i++)
            {
                if (td->Items[i].Group != g)
                    continue;
                SetRect(&lay->Cell[i], PAD + col * cw, y, PAD + (col + 1) * cw, y + ch);
                if (++col == cols)
                {
                    col = 0;
                    y += ch + 4;
                }
            }
            if (col)
                y += ch + 4;
        }
        else
        {
            for (i = 0; i < td->ItemCount; i++)
            {
                if (td->Items[i].Group == g)
                    SetRectEmpty(&lay->Cell[i]);
            }
        }
        y += 6;
        (void)n;
    }
    lay->Total = y;
}

static void
UpdateScroll(HWND hwnd, TGDATA *td, int total)
{
    RECT rc;
    SCROLLINFO si;
    int page;

    GetClientRect(hwnd, &rc);
    page = rc.bottom;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total > 0 ? total - 1 : 0;
    si.nPage = page;
    if (td->Scroll > total - page)
        td->Scroll = total - page;
    if (td->Scroll < 0)
        td->Scroll = 0;
    si.nPos = td->Scroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static TGLAYOUT *
GridLayout(HWND hwnd, TGDATA *td)
{
    TGLAYOUT *lay = (TGLAYOUT *)LocalAlloc(LPTR, sizeof(*lay));

    if (lay)
    {
        Layout(hwnd, td, lay);
        UpdateScroll(hwnd, td, lay->Total);
    }
    return lay;
}

HBITMAP
TgMakeThumb(HDC ref, const WCHAR *path, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HDC mem;
    HGDIOBJ old;
    GpBitmap *img = NULL;
    GpGraphics *g = NULL;
    UINT iw = 0, ih = 0;
    int sx, sy, sw, sh;
    GpStatus status;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp)
        return NULL;
    {
        DWORD *px = (DWORD *)bits;
        int n = w * h;

        /* opaque light grey under the picture so transparent pixels never show as black */
        while (n-- > 0)
            *px++ = 0xFFE6E6E6;
    }
    if (GdipCreateBitmapFromFile(path, &img) != Ok || !img)
    {
        DeleteObject(bmp);
        return NULL;
    }
    GdipGetImageWidth((GpImage *)img, &iw);
    GdipGetImageHeight((GpImage *)img, &ih);
    mem = CreateCompatibleDC(ref);
    old = SelectObject(mem, bmp);
    if (iw && ih && mem && GdipCreateFromHDC(mem, &g) == Ok)
    {
        /* cover: crop the picture to the thumbnail's aspect ratio */
        if ((__int64)iw * h > (__int64)ih * w)
        {
            sh = (int)ih;
            sw = (int)((__int64)ih * w / h);
            sx = ((int)iw - sw) / 2;
            sy = 0;
        }
        else
        {
            sw = (int)iw;
            sh = (int)((__int64)iw * h / w);
            sx = 0;
            sy = ((int)ih - sh) / 2;
        }
        /* big photos at high quality interpolation can fail inside GDI+; plain bilinear is enough at thumbnail size */
        GdipSetInterpolationMode(g, InterpolationModeBilinear);
        GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
        status = GdipDrawImageRectRectI(g, (GpImage *)img, 0, 0, w, h, sx, sy, sw, sh, UnitPixel, NULL, NULL, NULL);
        if (status != Ok)
        {
            /* fall back to a plain scaled copy of the whole picture */
            GdipDrawImageRectI(g, (GpImage *)img, 0, 0, w, h);
        }
        GdipDeleteGraphics(g);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    GdipDisposeImage((GpImage *)img);
    return bmp;
}

static void
DrawChevron(HDC hdc, int x, int y, BOOL open, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c), old = (HPEN)SelectObject(hdc, pen);

    if (open)
    {
        MoveToEx(hdc, x, y, NULL); LineTo(hdc, x + 4, y + 4); LineTo(hdc, x + 9, y - 1);
        MoveToEx(hdc, x, y + 1, NULL); LineTo(hdc, x + 4, y + 5); LineTo(hdc, x + 9, y);
    }
    else
    {
        MoveToEx(hdc, x + 1, y - 3, NULL); LineTo(hdc, x + 5, y + 1); LineTo(hdc, x + 1, y + 5);
        MoveToEx(hdc, x + 2, y - 3, NULL); LineTo(hdc, x + 6, y + 1); LineTo(hdc, x + 2, y + 5);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void
DrawCell(HWND hwnd, HDC hdc, TGDATA *td, int i, const RECT *cell)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    TGENTRY *e = &td->Items[i];
    BOOL sel = i == td->Selected, hot = i == td->Hot;
    RECT tr, lab;
    int cw = cell->right - cell->left;
    HBRUSH br;
    int tx;

    if (sel || hot)
    {
        COLORREF fill = sel ? RGB(205, 226, 247) : RGB(231, 241, 252);
        COLORREF line = sel ? RGB(125, 162, 206) : RGB(184, 214, 251);
        HBRUSH f = CreateSolidBrush(fill), l = CreateSolidBrush(line);

        FillRect(hdc, cell, f);
        FrameRect(hdc, cell, l);
        DeleteObject(f);
        DeleteObject(l);
    }
    tx = cell->left + (cw - td->ThumbW) / 2;
    SetRect(&tr, tx, cell->top + 6, tx + td->ThumbW, cell->top + 6 + td->ThumbH);
    if (!e->Thumb && !e->ThumbTried && e->Image[0])
    {
        e->ThumbTried = TRUE;
        e->Thumb = TgMakeThumb(hdc, e->Image, td->ThumbW, td->ThumbH);
    }
    if (e->Thumb)
    {
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, e->Thumb);

        BitBlt(hdc, tr.left, tr.top, td->ThumbW, td->ThumbH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    else
    {
        br = CreateSolidBrush(e->Fill);
        FillRect(hdc, &tr, br);
        DeleteObject(br);
    }
    br = CreateSolidBrush(RGB(150, 160, 175));
    FrameRect(hdc, &tr, br);
    DeleteObject(br);
    if ((style & TGS_SWATCH) && e->HasSwatch)
    {
        RECT sw;
        int s = td->ThumbH * 6 / 10;

        SetRect(&sw, tr.left + 6, tr.bottom - s - 4, tr.left + 6 + s, tr.bottom - 4);
        br = CreateSolidBrush(e->Swatch);
        FillRect(hdc, &sw, br);
        DeleteObject(br);
        br = CreateSolidBrush(RGB(255, 255, 255));
        FrameRect(hdc, &sw, br);
        DeleteObject(br);
        InflateRect(&sw, 1, 1);
        br = CreateSolidBrush(RGB(110, 120, 135));
        FrameRect(hdc, &sw, br);
        DeleteObject(br);
    }
    if ((style & TGS_CHECKBOXES) && (e->Checked || hot || sel))
    {
        RECT cb;

        SetRect(&cb, tr.left + 3, tr.top + 3, tr.left + 16, tr.top + 16);
        br = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &cb, br);
        DeleteObject(br);
        br = CreateSolidBrush(RGB(60, 90, 140));
        FrameRect(hdc, &cb, br);
        DeleteObject(br);
        if (e->Checked)
        {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(25, 90, 160)), old = (HPEN)SelectObject(hdc, pen);

            MoveToEx(hdc, cb.left + 3, cb.top + 6, NULL);
            LineTo(hdc, cb.left + 5, cb.top + 9);
            LineTo(hdc, cb.left + 10, cb.top + 3);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
    }
    if ((style & TGS_LABELS) && e->Text[0])
    {
        SetRect(&lab, cell->left + 2, tr.bottom + 4, cell->right - 2, cell->bottom - 2);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, e->Text, -1, &lab, DT_CENTER | DT_TOP | DT_END_ELLIPSIS | DT_WORDBREAK | DT_NOPREFIX);
    }
}

static void
Paint(HWND hwnd, TGDATA *td)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bmp;
    HGDIOBJ oldBmp, oldFont;
    RECT rc;
    TGLAYOUT *lay;
    int g, i;
    HBRUSH white;

    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    oldBmp = SelectObject(mem, bmp);
    white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mem, &rc, white);
    DeleteObject(white);
    oldFont = SelectObject(mem, td->Font);
    lay = GridLayout(hwnd, td);
    if (lay)
    {
        SetViewportOrgEx(mem, 0, -td->Scroll, NULL);
        for (g = 0; g < td->GroupCount; g++)
        {
            RECT h = lay->Header[g], t;
            WCHAR title[100];
            int n = 0;
            SIZE sz;
            HPEN pen;
            HGDIOBJ old;

            for (i = 0; i < td->ItemCount; i++)
                n += td->Items[i].Group == g;
            StringCchPrintfW(title, ARRAYSIZE(title), L"%s (%d)", td->Groups[g].Name, n);
            DrawChevron(mem, h.left + 4, h.top + HEADER_H / 2 - 2, !td->Groups[g].Collapsed, RGB(30, 80, 150));
            t = h;
            t.left += 20;
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(30, 57, 91));
            DrawTextW(mem, title, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GetTextExtentPoint32W(mem, title, lstrlenW(title), &sz);
            pen = CreatePen(PS_SOLID, 1, RGB(200, 210, 225));
            old = SelectObject(mem, pen);
            MoveToEx(mem, h.left + 20 + sz.cx + 8, h.top + HEADER_H / 2, NULL);
            LineTo(mem, h.right, h.top + HEADER_H / 2);
            SelectObject(mem, old);
            DeleteObject(pen);
        }
        for (i = 0; i < td->ItemCount; i++)
        {
            RECT c = lay->Cell[i];

            if (IsRectEmpty(&c) || c.bottom - td->Scroll < 0 || c.top - td->Scroll > rc.bottom)
                continue;
            DrawCell(hwnd, mem, td, i, &c);
        }
        SetViewportOrgEx(mem, 0, 0, NULL);
        LocalFree(lay);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* 0 = nothing, 1 = item (idx), 2 = header (idx), 3 = checkbox of item (idx) */
static int
HitTest(HWND hwnd, TGDATA *td, int x, int y, int *idx)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    TGLAYOUT *lay = GridLayout(hwnd, td);
    int g, i, r = 0;
    POINT pt;

    pt.x = x;
    pt.y = y + td->Scroll;
    *idx = -1;
    if (!lay)
        return 0;
    for (g = 0; g < td->GroupCount && !r; g++)
    {
        if (PtInRect(&lay->Header[g], pt))
        {
            *idx = g;
            r = 2;
        }
    }
    for (i = 0; i < td->ItemCount && !r; i++)
    {
        if (!IsRectEmpty(&lay->Cell[i]) && PtInRect(&lay->Cell[i], pt))
        {
            int cw = lay->Cell[i].right - lay->Cell[i].left;
            int tx = lay->Cell[i].left + (cw - td->ThumbW) / 2;

            *idx = i;
            r = 1;
            if ((style & TGS_CHECKBOXES) && pt.x >= tx + 3 && pt.x <= tx + 16 &&
                pt.y >= lay->Cell[i].top + 9 && pt.y <= lay->Cell[i].top + 22)
                r = 3;
        }
    }
    LocalFree(lay);
    return r;
}

static void
Notify(HWND hwnd, UINT code)
{
    HWND parent = GetParent(hwnd);

    if (parent)
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), code), (LPARAM)hwnd);
}

static void
EnsureVisible(HWND hwnd, TGDATA *td, int i)
{
    TGLAYOUT *lay;
    RECT rc;

    if (i < 0 || i >= td->ItemCount)
        return;
    lay = GridLayout(hwnd, td);
    if (!lay)
        return;
    GetClientRect(hwnd, &rc);
    if (!IsRectEmpty(&lay->Cell[i]))
    {
        if (lay->Cell[i].top < td->Scroll)
            td->Scroll = lay->Cell[i].top;
        else if (lay->Cell[i].bottom > td->Scroll + rc.bottom)
            td->Scroll = lay->Cell[i].bottom - rc.bottom;
    }
    LocalFree(lay);
}

static void
SetScroll(HWND hwnd, TGDATA *td, int pos)
{
    td->Scroll = pos;
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
}

static LRESULT CALLBACK
GridProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TGDATA *td = (TGDATA *)GetWindowLongPtrW(hwnd, 0);

    switch (msg)
    {
        case WM_NCCREATE:
        {
            NONCLIENTMETRICSW ncm;

            td = (TGDATA *)LocalAlloc(LPTR, sizeof(*td));
            if (!td)
                return FALSE;
            td->Items = (TGENTRY *)LocalAlloc(LPTR, sizeof(TGENTRY) * TG_MAX_ITEMS);
            if (!td->Items)
            {
                LocalFree(td);
                return FALSE;
            }
            td->Selected = -1;
            td->Hot = -1;
            td->ThumbW = Dpi(hwnd, 96);
            td->ThumbH = Dpi(hwnd, 54);
            ncm.cbSize = sizeof(ncm);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            {
                td->Font = CreateFontIndirectW(&ncm.lfMessageFont);
                td->OwnFont = td->Font != NULL;
            }
            if (!td->Font)
                td->Font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            SetWindowLongPtrW(hwnd, 0, (LONG_PTR)td);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_NCDESTROY:
            if (td)
            {
                int i;

                for (i = 0; i < td->ItemCount; i++)
                {
                    if (td->Items[i].Thumb)
                        DeleteObject(td->Items[i].Thumb);
                }
                if (td->OwnFont)
                    DeleteObject(td->Font);
                LocalFree(td->Items);
                LocalFree(td);
                SetWindowLongPtrW(hwnd, 0, 0);
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (td)
            {
                Paint(hwnd, td);
                return 0;
            }
            break;
        case WM_SIZE:
            if (td)
                InvalidateRect(hwnd, NULL, FALSE);
            break;
        case WM_VSCROLL:
            if (td)
            {
                SCROLLINFO si;
                int pos = td->Scroll, line = td->ThumbH / 2 + 8;

                ZeroMemory(&si, sizeof(si));
                si.cbSize = sizeof(si);
                si.fMask = SIF_ALL;
                GetScrollInfo(hwnd, SB_VERT, &si);
                switch (LOWORD(wParam))
                {
                    case SB_LINEUP: pos -= line; break;
                    case SB_LINEDOWN: pos += line; break;
                    case SB_PAGEUP: pos -= (int)si.nPage; break;
                    case SB_PAGEDOWN: pos += (int)si.nPage; break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                    case SB_TOP: pos = 0; break;
                    case SB_BOTTOM: pos = si.nMax; break;
                }
                SetScroll(hwnd, td, pos);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (td)
            {
                int delta = (short)HIWORD(wParam);

                SetScroll(hwnd, td, td->Scroll - delta * (td->ThumbH + 20) / (WHEEL_DELTA * 2));
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (td)
            {
                int idx, what = HitTest(hwnd, td, (short)LOWORD(lParam), (short)HIWORD(lParam), &idx);
                int hot = (what == 1 || what == 3) ? idx : -1;

                if (!td->Tracking)
                {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

                    td->Tracking = TrackMouseEvent(&tme);
                }
                if (hot != td->Hot)
                {
                    td->Hot = hot;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        case WM_MOUSELEAVE:
            if (td)
            {
                td->Tracking = FALSE;
                if (td->Hot != -1)
                {
                    td->Hot = -1;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        case WM_LBUTTONDOWN:
            if (td)
            {
                int idx, what = HitTest(hwnd, td, (short)LOWORD(lParam), (short)HIWORD(lParam), &idx);

                SetFocus(hwnd);
                if (what == 2)
                {
                    td->Groups[idx].Collapsed = !td->Groups[idx].Collapsed;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                else if (what == 1 || what == 3)
                {
                    BOOL changed = td->Selected != idx;

                    td->Selected = idx;
                    if (what == 3)
                    {
                        td->Items[idx].Checked = !td->Items[idx].Checked;
                        Notify(hwnd, TGN_CHECK);
                    }
                    if (changed)
                        Notify(hwnd, TGN_SEL);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (td)
            {
                int idx, what = HitTest(hwnd, td, (short)LOWORD(lParam), (short)HIWORD(lParam), &idx);

                if (what == 1 || what == 3)
                {
                    td->Selected = idx;
                    Notify(hwnd, TGN_ACTIVATE);
                }
                return 0;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (td && td->ItemCount)
            {
                int cur = td->Selected < 0 ? 0 : td->Selected, next = cur;

                switch (wParam)
                {
                    case VK_RIGHT: case VK_DOWN: next = cur + 1; break;
                    case VK_LEFT: case VK_UP: next = cur - 1; break;
                    case VK_HOME: next = 0; break;
                    case VK_END: next = td->ItemCount - 1; break;
                    case VK_RETURN: Notify(hwnd, TGN_ACTIVATE); return 0;
                    case VK_SPACE:
                        if (td->Selected >= 0 && (GetWindowLongW(hwnd, GWL_STYLE) & TGS_CHECKBOXES))
                        {
                            td->Items[td->Selected].Checked = !td->Items[td->Selected].Checked;
                            Notify(hwnd, TGN_CHECK);
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                        return 0;
                    default: return 0;
                }
                if (next < 0)
                    next = 0;
                if (next >= td->ItemCount)
                    next = td->ItemCount - 1;
                if (next != td->Selected)
                {
                    td->Selected = next;
                    EnsureVisible(hwnd, td, next);
                    Notify(hwnd, TGN_SEL);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            break;
        case TGM_CLEAR:
            if (td)
            {
                int i;

                for (i = 0; i < td->ItemCount; i++)
                {
                    if (td->Items[i].Thumb)
                        DeleteObject(td->Items[i].Thumb);
                }
                ZeroMemory(td->Items, sizeof(TGENTRY) * TG_MAX_ITEMS);
                td->ItemCount = td->GroupCount = 0;
                td->Selected = td->Hot = -1;
                td->Scroll = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case TGM_ADDGROUP:
            if (td && td->GroupCount < TG_MAX_GROUPS)
            {
                lstrcpynW(td->Groups[td->GroupCount].Name, (const WCHAR *)lParam, ARRAYSIZE(td->Groups[0].Name));
                td->Groups[td->GroupCount].Collapsed = FALSE;
                return td->GroupCount++;
            }
            return -1;
        case TGM_ADDITEM:
            if (td && td->ItemCount < TG_MAX_ITEMS && lParam)
            {
                const TGITEM *in = (const TGITEM *)lParam;
                TGENTRY *e = &td->Items[td->ItemCount];

                if (in->Group < 0 || in->Group >= td->GroupCount)
                    return -1;
                e->Group = in->Group;
                lstrcpynW(e->Text, in->Text ? in->Text : L"", ARRAYSIZE(e->Text));
                lstrcpynW(e->Image, in->Image ? in->Image : L"", ARRAYSIZE(e->Image));
                e->Fill = in->Fill;
                e->HasSwatch = in->HasSwatch;
                e->Swatch = in->Swatch;
                e->Checked = in->Checked;
                e->Data = in->Data;
                InvalidateRect(hwnd, NULL, FALSE);
                return td->ItemCount++;
            }
            return -1;
        case TGM_SETSEL:
            if (td)
            {
                td->Selected = ((int)wParam >= 0 && (int)wParam < td->ItemCount) ? (int)wParam : -1;
                EnsureVisible(hwnd, td, td->Selected);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case TGM_GETSEL:
            return td ? td->Selected : -1;
        case TGM_SETCHECK:
            if (td && (int)wParam >= 0 && (int)wParam < td->ItemCount)
            {
                td->Items[wParam].Checked = lParam != 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case TGM_GETCHECK:
            return (td && (int)wParam >= 0 && (int)wParam < td->ItemCount) ? td->Items[wParam].Checked : FALSE;
        case TGM_GETCOUNT:
            return td ? td->ItemCount : 0;
        case TGM_CHECKALL:
            if (td)
            {
                int i;

                for (i = 0; i < td->ItemCount; i++)
                    td->Items[i].Checked = lParam != 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case TGM_GETDATA:
            return (td && (int)wParam >= 0 && (int)wParam < td->ItemCount) ? td->Items[wParam].Data : 0;
        case TGM_GETIMAGE:
            if (td && (int)wParam >= 0 && (int)wParam < td->ItemCount && lParam)
            {
                lstrcpynW((WCHAR *)lParam, td->Items[wParam].Image, MAX_PATH);
                return TRUE;
            }
            return FALSE;
        case TGM_SETCELL:
            if (td)
            {
                td->ThumbW = (int)wParam;
                td->ThumbH = (int)lParam;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL
TgRegister(HINSTANCE hInst)
{
    WNDCLASSEXW wc;

    if (GetClassInfoExW(hInst, TG_CLASS, &wc))
        return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = GridProc;
    wc.cbWndExtra = sizeof(LONG_PTR);
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = TG_CLASS;
    return RegisterClassExW(&wc) != 0;
}
