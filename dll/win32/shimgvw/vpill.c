/*
 * PROJECT:     LiberNT Photo Viewer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Win7-style chrome: text menu strip on top and the floating pill toolbar
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The strip is an owner-drawn child with text buttons (File, Print, E-mail, Burn,
 * Open); the dropdowns are plain popup menus. The pill is a layered popup owned by
 * the main window, painted with GDI+ into a premultiplied DIB (anti-aliased capsule,
 * drop shadow, procedural glyphs), so it floats over the image on every Windows
 * that has UpdateLayeredWindow and never needs a layered child.
 */

#include "shimgvw.h"
#include <windowsx.h>
#include <commctrl.h>
#include <math.h>
#include "vpill.h"
#include "vribbon_model.h"

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define STRIP_CLASS L"ShImgVw:CMenuStrip"
#define PILL_CLASS  L"ShImgVw:CPillBar"
#define SLIDER_CLASS L"ShImgVw:CPillZoom"

/* Pill items, left to right */
enum { PI_ZOOM, PI_FIT, PI_PREV, PI_PLAY, PI_NEXT, PI_CCW, PI_CW, PI_DEL, PI_COUNT };
static const UINT s_ItemCmd[PI_COUNT] =
{
    0, IDC_BEST_FIT, IDC_PREV_PIC, IDC_SLIDE_SHOW, IDC_NEXT_PIC,
    IDC_ROT_COUNCW, IDC_ROT_CLOCKW, IDC_DELETE
};

typedef struct StripItem
{
    UINT idsLabel;
    BOOL dropdown;
    RECT rc;
} StripItem;

struct VPILL
{
    HWND main, strip, pill, slider, tip;
    HINSTANCE inst;
    VPILL_HOST host;
    UINT dpi;
    HFONT font;
    StripItem items[5];
    int hotStrip, openStrip;
    RECT itemRc[PI_COUNT];
    RECT capsule;           /* inside the layered window, shadow excluded */
    SIZE size;              /* layered window size */
    int hotPill, downPill;
    BOOL tracking;
    POINT origin;           /* last pill position, screen */
    GpBitmap *icon[PI_COUNT + 1];   /* Aureline art per slot, [PI_COUNT] = the 'actual size' state */
    int lastCx, lastCy;     /* last layout size, to re-place the pill after a switch */
    BOOL overlay;           /* slideshow overlay: prev / end / next only, no strip */
    BOOL needPlain;         /* UpdateLayeredWindow failed: switch to the plain pill */
    BOOL plain;             /* no layered windows: opaque capsule-shaped popup, no shadow */
    int margin;             /* shadow margin around the capsule (0 when plain) */
    HBITMAP hbmSnap;        /* the last rendered pill (premultiplied), for WM_PRINT */
};

static ATOM s_atoms[3];
static VPILL *s_active;     /* single instance: used by the slider popup */

/* ---- helpers ---- */

static UINT
GetDpiFor(HWND hwnd)
{
    typedef UINT (WINAPI *FN)(HWND);
    static FN fn;
    static BOOL looked;
    UINT dpi = 0;
    if (!looked)
    {
        HMODULE h = GetModuleHandleW(L"user32.dll");
        fn = h ? (FN)GetProcAddress(h, "GetDpiForWindow") : NULL;
        looked = TRUE;
    }
    if (fn)
        dpi = fn(hwnd);
    if (!dpi)
    {
        HDC hdc = GetDC(NULL);
        dpi = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (hdc)
            ReleaseDC(NULL, hdc);
    }
    return dpi ? dpi : 96;
}

static int
Sc(VPILL *p, int v)
{
    return MulDiv(v, (int)p->dpi, 96);
}

static void
LoadLabel(VPILL *p, UINT ids, const WCHAR *fallback, WCHAR *buf, int cch)
{
    if (!LoadStringW(p->inst, ids, buf, cch))
        StringCchCopyW(buf, cch, fallback);
}

static const WCHAR *
DefaultText(UINT ids)
{
    switch (ids)
    {
        case IDS_MENU_FILE: return L"File";
        case IDS_MENU_PRINT: return L"Print";
        case IDS_MENU_EMAIL: return L"E-mail";
        case IDS_MENU_BURN: return L"Burn";
        case IDS_MENU_OPEN: return L"Open";
        case IDS_ITEM_OPEN: return L"Open...";
        case IDS_ITEM_SAVEAS: return L"Save as...";
        case IDS_ITEM_COPY: return L"Copy";
        case IDS_ITEM_COPYPATH: return L"Copy file path";
        case IDS_ITEM_DELETE: return L"Delete";
        case IDS_ITEM_WALLPAPER: return L"Set as desktop background";
        case IDS_ITEM_PROPS: return L"Properties";
        case IDS_ITEM_EXIT: return L"Exit";
        case IDS_ITEM_PRINT: return L"Print...";
        case IDS_ITEM_BURN: return L"Burn to disc...";
        case IDS_ITEM_EDIT: return L"Edit";
        case IDS_ITEM_OPENWITH: return L"Open with...";
        case IDS_TOOLTIP_DISPLAYSIZE: return L"Change the display size";
        case IDS_TOOLTIP_FIT_TOGGLE: return L"Fit to window / actual size";
    }
    return L"";
}

static void
Text(VPILL *p, UINT ids, WCHAR *buf, int cch)
{
    LoadLabel(p, ids, DefaultText(ids), buf, cch);
}

/* ---- GDI+ drawing ---- */

#define ARGB(a, r, g, b) ((ARGB_T)(((UINT)(a) << 24) | ((UINT)(r) << 16) | ((UINT)(g) << 8) | (UINT)(b)))
typedef UINT ARGB_T;

static void
RRectPath(GpPath **pp, float x, float y, float w, float h, float r)
{
    GpPath *path;
    float d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    GdipCreatePath(FillModeAlternate, &path);
    GdipAddPathArc(path, x, y, d, d, 180, 90);
    GdipAddPathArc(path, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(path, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    *pp = path;
}

static void
FillRRect(GpGraphics *g, float x, float y, float w, float h, float r, GpBrush *brush, GpPen *pen)
{
    GpPath *path;
    RRectPath(&path, x, y, w, h, r);
    if (brush)
        GdipFillPath(g, brush, path);
    if (pen)
        GdipDrawPath(g, pen, path);
    GdipDeletePath(path);
}

static GpBrush *
VGrad(float x, float y, float h, ARGB_T top, ARGB_T bot)
{
    GpLineGradient *lg = NULL;
    GpPointF p1 = { x, y }, p2 = { x, y + h };
    GdipCreateLineBrush(&p1, &p2, top, bot, WrapModeTileFlipXY, &lg);
    return (GpBrush *)lg;
}

static void
Disc(GpGraphics *g, float cx, float cy, float d, GpBrush *brush, GpPen *pen)
{
    if (brush)
        GdipFillEllipse(g, brush, cx - d / 2, cy - d / 2, d, d);
    if (pen)
        GdipDrawEllipse(g, pen, cx - d / 2, cy - d / 2, d, d);
}

static GpPen *
MakePen(ARGB_T color, float width)
{
    GpPen *pen = NULL;
    GdipCreatePen1(color, width, UnitPixel, &pen);
    if (pen)
    {
        GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapRound);
        GdipSetPenLineJoin(pen, LineJoinRound);
    }
    return pen;
}

static void
DrawChevron(GpGraphics *g, float cx, float cy, float s, BOOL right, ARGB_T color, float w)
{
    GpPen *pen = MakePen(color, w);
    GpPointF pt[3];
    float dir = right ? 1.f : -1.f;
    pt[0].X = cx - dir * s * 0.28f; pt[0].Y = cy - s * 0.42f;
    pt[1].X = cx + dir * s * 0.22f; pt[1].Y = cy;
    pt[2].X = cx - dir * s * 0.28f; pt[2].Y = cy + s * 0.42f;
    GdipDrawLines(g, pen, pt, 3);
    GdipDeletePen(pen);
}

static void
DrawRotate(GpGraphics *g, float cx, float cy, float s, BOOL clockwise, ARGB_T color, float w)
{
    GpPen *pen = MakePen(color, w);
    GpBrush *br = NULL;
    float r = s * 0.40f;
    GpPointF tri[3];
    float a;
    /* an open circle with a gap at the top, the arrow head sits at the gap */
    if (clockwise)
    {
        GdipDrawArc(g, pen, cx - r, cy - r, r * 2, r * 2, -60, 290);
        a = 230.f * 3.14159265f / 180.f;
        {
            float px = cx + r * cosf(-60.f * 3.14159265f / 180.f), py = cy + r * sinf(-60.f * 3.14159265f / 180.f);
            /* head at the start of the arc, pointing along the direction of travel (counter to the sweep start) */
            tri[0].X = px + s * 0.26f; tri[0].Y = py - s * 0.02f;
            tri[1].X = px - s * 0.04f; tri[1].Y = py - s * 0.30f;
            tri[2].X = px - s * 0.12f; tri[2].Y = py + s * 0.22f;
        }
        (void)a;
    }
    else
    {
        GdipDrawArc(g, pen, cx - r, cy - r, r * 2, r * 2, 240, -290);
        {
            float px = cx + r * cosf(240.f * 3.14159265f / 180.f), py = cy + r * sinf(240.f * 3.14159265f / 180.f);
            tri[0].X = px - s * 0.26f; tri[0].Y = py - s * 0.02f;
            tri[1].X = px + s * 0.04f; tri[1].Y = py - s * 0.30f;
            tri[2].X = px + s * 0.12f; tri[2].Y = py + s * 0.22f;
        }
    }
    GdipCreateSolidFill(color, (GpSolidFill **)&br);
    GdipFillPolygon(g, br, tri, 3, FillModeAlternate);
    GdipDeleteBrush(br);
    GdipDeletePen(pen);
}

static void
DrawGlyph(GpGraphics *g, int item, float cx, float cy, float s, BOOL enabled, BOOL fitMode, BOOL hot)
{
    UINT a = enabled ? 255 : 110;
    ARGB_T ink = ARGB(a, 0x3B, 0x46, 0x50);
    ARGB_T blue = ARGB(a, 0x1F, 0x5F, 0xB8);
    ARGB_T red = ARGB(a, 0xB8, 0x32, 0x2B);
    float w = s * 0.085f;
    GpPen *pen;
    GpBrush *br = NULL;

    if (w < 1.5f) w = 1.5f;
    (void)hot;
    switch (item)
    {
        case PI_ZOOM:
        {
            /* magnifier, with a small dropdown arrow on its right */
            float x = cx - s * 0.12f, y = cy - s * 0.10f, r = s * 0.27f;
            GpPointF ar[3];
            pen = MakePen(ink, w);
            GdipDrawEllipse(g, pen, x - r, y - r, r * 2, r * 2);
            GdipDeletePen(pen);
            pen = MakePen(ink, w * 1.6f);
            GdipDrawLine(g, pen, x + r * 0.72f, y + r * 0.72f, x + r * 1.55f, y + r * 1.55f);
            GdipDeletePen(pen);
            pen = MakePen(blue, w * 0.8f);
            GdipDrawLine(g, pen, x - r * 0.5f, y, x + r * 0.5f, y);
            GdipDrawLine(g, pen, x, y - r * 0.5f, x, y + r * 0.5f);
            GdipDeletePen(pen);
            ar[0].X = cx + s * 0.40f; ar[0].Y = cy - s * 0.04f;
            ar[1].X = cx + s * 0.62f; ar[1].Y = cy - s * 0.04f;
            ar[2].X = cx + s * 0.51f; ar[2].Y = cy + s * 0.09f;
            GdipCreateSolidFill(ink, (GpSolidFill **)&br);
            GdipFillPolygon(g, br, ar, 3, FillModeAlternate);
            GdipDeleteBrush(br);
            break;
        }
        case PI_FIT:
        {
            float h = s * 0.34f, k = s * 0.16f;
            pen = MakePen(ink, w);
            if (!fitMode)
            {
                /* four outward corners: fit the picture to the window */
                float x0 = cx - h, y0 = cy - h * 0.8f, x1 = cx + h, y1 = cy + h * 0.8f;
                GdipDrawLine(g, pen, x0, y0 + k, x0, y0); GdipDrawLine(g, pen, x0, y0, x0 + k, y0);
                GdipDrawLine(g, pen, x1 - k, y0, x1, y0); GdipDrawLine(g, pen, x1, y0, x1, y0 + k);
                GdipDrawLine(g, pen, x0, y1 - k, x0, y1); GdipDrawLine(g, pen, x0, y1, x0 + k, y1);
                GdipDrawLine(g, pen, x1 - k, y1, x1, y1); GdipDrawLine(g, pen, x1, y1, x1, y1 - k);
                GdipDeletePen(pen);
                pen = MakePen(blue, w);
                GdipDrawRectangle(g, pen, cx - h * 0.45f, cy - h * 0.36f, h * 0.9f, h * 0.72f);
            }
            else
            {
                /* a one-to-one tile: actual size */
                GpFontFamily *fam = NULL;
                GpFont *font = NULL;
                GpStringFormat *fmt = NULL;
                RectF rc;
                GdipDrawRectangle(g, pen, cx - h * 0.95f, cy - h * 0.75f, h * 1.9f, h * 1.5f);
                GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
                if (fam)
                {
                    GdipCreateFont(fam, s * 0.30f, FontStyleBold, UnitPixel, &font);
                    GdipCreateStringFormat(0, 0, &fmt);
                    if (font && fmt)
                    {
                        GdipSetStringFormatAlign(fmt, StringAlignmentCenter);
                        GdipSetStringFormatLineAlign(fmt, StringAlignmentCenter);
                        rc.X = cx - h; rc.Y = cy - h * 0.75f; rc.Width = h * 2; rc.Height = h * 1.5f;
                        GdipCreateSolidFill(blue, (GpSolidFill **)&br);
                        GdipDrawString(g, L"1:1", 3, font, &rc, fmt, br);
                        GdipDeleteBrush(br);
                    }
                    if (fmt) GdipDeleteStringFormat(fmt);
                    if (font) GdipDeleteFont(font);
                    GdipDeleteFontFamily(fam);
                }
            }
            GdipDeletePen(pen);
            break;
        }
        case PI_PREV:
            DrawChevron(g, cx, cy, s, FALSE, ink, w * 1.3f);
            break;
        case PI_NEXT:
            DrawChevron(g, cx, cy, s, TRUE, ink, w * 1.3f);
            break;
        case PI_PLAY:
        {
            GpPointF tri[3];
            tri[0].X = cx - s * 0.20f; tri[0].Y = cy - s * 0.30f;
            tri[1].X = cx + s * 0.32f; tri[1].Y = cy;
            tri[2].X = cx - s * 0.20f; tri[2].Y = cy + s * 0.30f;
            GdipCreateSolidFill(ARGB(a, 255, 255, 255), (GpSolidFill **)&br);
            GdipFillPolygon(g, br, tri, 3, FillModeAlternate);
            GdipDeleteBrush(br);
            break;
        }
        case PI_CCW:
            DrawRotate(g, cx, cy, s, FALSE, ink, w);
            break;
        case PI_CW:
            DrawRotate(g, cx, cy, s, TRUE, ink, w);
            break;
        case PI_DEL:
        {
            float d = s * 0.27f;
            pen = MakePen(red, w * 1.7f);
            GdipDrawLine(g, pen, cx - d, cy - d, cx + d, cy + d);
            GdipDrawLine(g, pen, cx - d, cy + d, cx + d, cy - d);
            GdipDeletePen(pen);
            break;
        }
    }
}

/* ---- Aureline icons: icons/viewer/aur.viewer.<slug>.ico next to the module, 32 px frame ---- */

static GpBitmap *
LoadIcon32(VPILL *p, const WCHAR *slug)
{
    WCHAR path[MAX_PATH];
    HICON hi;
    GpBitmap *bmp = NULL;
    WCHAR *slash;

    if (!GetModuleFileNameW(p->inst, path, MAX_PATH))
        return NULL;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return NULL;
    *slash = 0;
    if (FAILED(StringCchCatW(path, MAX_PATH, L"\\icons\\viewer\\aur.viewer.")) ||
        FAILED(StringCchCatW(path, MAX_PATH, slug)) || FAILED(StringCchCatW(path, MAX_PATH, L".ico")))
        return NULL;
    hi = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    if (!hi)
        return NULL;
    GdipCreateBitmapFromHICON(hi, &bmp);
    DestroyIcon(hi);
    return bmp;
}

static void
LoadIcons(VPILL *p)
{
    static const WCHAR *slugs[PI_COUNT + 1] =
        { L"zoom", L"fit", L"prev", L"slideshow", L"next", L"rotate-ccw", L"rotate-cw", L"delete", L"actual" };
    int i;
    for (i = 0; i < PI_COUNT + 1; i++)
        p->icon[i] = LoadIcon32(p, slugs[i]);
}

static void
DrawPillIcon(GpGraphics *g, GpBitmap *bmp, float cx, float cy, float size, BOOL enabled)
{
    GpImageAttributes *ia = NULL;
    ColorMatrix cm;

    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    if (!enabled)
    {
        ZeroMemory(&cm, sizeof(cm));
        cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = cm.m[4][4] = 1.f;
        cm.m[3][3] = 0.45f;
        GdipCreateImageAttributes(&ia);
        GdipSetImageAttributesColorMatrix(ia, ColorAdjustTypeBitmap, TRUE, &cm, NULL, ColorMatrixFlagsDefault);
    }
    GdipDrawImageRectRect(g, (GpImage *)bmp, cx - size / 2, cy - size / 2, size, size, 0, 0, 32, 32, UnitPixel, ia, NULL, NULL);
    if (ia)
        GdipDisposeImageAttributes(ia);
}

/* ---- pill geometry and painting ---- */

static void
PillMetrics(VPILL *p)
{
    int M = p->margin, H = Sc(p, 50), pad = Sc(p, 16);
    int small = Sc(p, 34), zoomW = Sc(p, 50), nav = Sc(p, 36), play = Sc(p, 48);
    int gap = Sc(p, 2), sep = Sc(p, 16);
    int x, leftW, rightW, extra;
    int y = M + (H - small) / 2;

    if (p->overlay)
    {
        ZeroMemory(p->itemRc, sizeof(p->itemRc));
        x = M + pad;
        y = M + (H - nav) / 2;
        SetRect(&p->itemRc[PI_PREV], x, y, x + nav, y + nav); x += nav + gap;
        y = M + (H - play) / 2;
        SetRect(&p->itemRc[PI_PLAY], x, y, x + play, y + play); x += play + gap;
        y = M + (H - nav) / 2;
        SetRect(&p->itemRc[PI_NEXT], x, y, x + nav, y + nav); x += nav;
        SetRect(&p->capsule, M, M, x + pad, M + H);
        p->size.cx = p->capsule.right + M;
        p->size.cy = p->capsule.bottom + M;
        return;
    }

    leftW = zoomW + gap + small;
    rightW = small * 3 + gap * 2;
    extra = rightW - leftW;
    if (extra < 0)
        extra = 0;
    x = M + pad + extra;
    SetRect(&p->itemRc[PI_ZOOM], x, y, x + zoomW, y + small); x += zoomW + gap;
    SetRect(&p->itemRc[PI_FIT], x, y, x + small, y + small); x += small + sep;
    y = M + (H - nav) / 2;
    SetRect(&p->itemRc[PI_PREV], x, y, x + nav, y + nav); x += nav + gap;
    y = M + (H - play) / 2;
    SetRect(&p->itemRc[PI_PLAY], x, y, x + play, y + play); x += play + gap;
    y = M + (H - nav) / 2;
    SetRect(&p->itemRc[PI_NEXT], x, y, x + nav, y + nav); x += nav + sep;
    y = M + (H - small) / 2;
    SetRect(&p->itemRc[PI_CCW], x, y, x + small, y + small); x += small + gap;
    SetRect(&p->itemRc[PI_CW], x, y, x + small, y + small); x += small + gap;
    SetRect(&p->itemRc[PI_DEL], x, y, x + small, y + small); x += small;
    SetRect(&p->capsule, M, M, x + pad, M + H);
    p->size.cx = p->capsule.right + M;
    p->size.cy = p->capsule.bottom + M;
}

static BOOL
ItemEnabled(VPILL *p, int i)
{
    BOOL hasImg = p->host.HasImage ? p->host.HasImage(p->host.ctx) : TRUE;
    switch (i)
    {
        case PI_ZOOM: case PI_CCW: case PI_CW: return hasImg;
        case PI_FIT: return hasImg;
        case PI_PREV: case PI_NEXT: case PI_PLAY: return hasImg;
        case PI_DEL: return hasImg && p->host.IsEnabled(p->host.ctx, IDC_DELETE);
    }
    return TRUE;
}

static void RenderPill(VPILL *p);
static void PillTipSync(VPILL *p);
static HWND CreatePillWindow(VPILL *p);
static void PillMetrics(VPILL *p);

static void
SwitchToPlain(VPILL *p)
{
    if (p->plain)
        return;
    if (p->slider)
        DestroyWindow(p->slider);
    if (p->tip)
        DestroyWindow(p->tip);
    DestroyWindow(p->pill);
    p->plain = TRUE;
    p->margin = 0;
    p->pill = CreatePillWindow(p);
    p->tip = p->pill ? CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                       0, 0, 0, 0, p->pill, NULL, p->inst, NULL) : NULL;
    PillMetrics(p);
    PillTipSync(p);
    VPill_Layout(p, p->lastCx, p->lastCy);
}

static void
RenderPill(VPILL *p)
{
    BITMAPINFO bi;
    HDC hdcScreen, hdcMem;
    HBITMAP hbm, hbmOld;
    void *bits = NULL;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 }, ptDst;
    BOOL fit = p->host.IsFit ? p->host.IsFit(p->host.ctx) : FALSE;
    float M = (float)p->margin, cw = (float)(p->capsule.right - p->capsule.left), ch = (float)(p->capsule.bottom - p->capsule.top);
    float rad = ch / 2;
    int i, k;
    RECT wr;

    PillMetrics(p);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = p->size.cx;
    bi.bmiHeader.biHeight = -p->size.cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hdcScreen = GetDC(NULL);
    hdcMem = CreateCompatibleDC(hdcScreen);
    hbm = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits)
        goto done;
    hbmOld = SelectObject(hdcMem, hbm);
    ZeroMemory(bits, (size_t)p->size.cx * p->size.cy * 4);

    if (GdipCreateBitmapFromScan0(p->size.cx, p->size.cy, p->size.cx * 4, PixelFormat32bppPARGB, bits, &bmp) == Ok &&
        GdipGetImageGraphicsContext((GpImage *)bmp, &g) == Ok)
    {
        GpBrush *br;
        GpPen *pen;

        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);

        /* drop shadow: stacked translucent capsules, widest first */
        GdipGraphicsClear(g, p->plain ? ARGB(255, 0xE4, 0xE8, 0xEE) : ARGB(0, 0, 0, 0));
        for (k = (int)M; k >= 1; k--)
        {
            br = NULL;
            GdipCreateSolidFill(ARGB(5, 0, 0, 0), (GpSolidFill **)&br);
            FillRRect(g, M - k, M - k + Sc(p, 3), cw + 2 * k, ch + 2 * k, rad + k, br, NULL);
            GdipDeleteBrush(br);
        }
        /* body */
        br = VGrad(M, M, ch, ARGB(245, 0xFF, 0xFF, 0xFF), ARGB(245, 0xD9, 0xE1, 0xEB));
        pen = MakePen(ARGB(235, 0x7F, 0x8C, 0x9C), 1.f);
        FillRRect(g, M, M, cw, ch, rad, br, pen);
        GdipDeleteBrush(br);
        GdipDeletePen(pen);
        /* glossy top highlight */
        br = VGrad(M, M, ch * 0.5f, ARGB(150, 255, 255, 255), ARGB(0, 255, 255, 255));
        FillRRect(g, M + 2, M + 1.5f, cw - 4, ch * 0.5f, rad - 2, br, NULL);
        GdipDeleteBrush(br);

        /* separators */
        pen = MakePen(ARGB(120, 0x7F, 0x8C, 0x9C), 1.f);
        if (!p->overlay)
        {
            float sy0 = M + ch * 0.26f, sy1 = M + ch * 0.74f;
            float sx1 = (p->itemRc[PI_FIT].right + p->itemRc[PI_PREV].left) / 2.f;
            float sx2 = (p->itemRc[PI_NEXT].right + p->itemRc[PI_CCW].left) / 2.f;
            GdipDrawLine(g, pen, sx1, sy0, sx1, sy1);
            GdipDrawLine(g, pen, sx2, sy0, sx2, sy1);
        }
        GdipDeletePen(pen);

        for (i = 0; i < PI_COUNT; i++)
        {
            const RECT *r = &p->itemRc[i];
            if (IsRectEmpty(r))
                continue;
            float cx = (r->left + r->right) / 2.f, cy = (r->top + r->bottom) / 2.f;
            float d = (float)(r->bottom - r->top);
            BOOL en = ItemEnabled(p, i);
            BOOL hot = (i == p->hotPill) && en;
            BOOL down = hot && (i == p->downPill);

            GpBitmap *ico = p->icon[(i == PI_FIT && fit) ? PI_COUNT : i];

            if (ico)
            {
                if (hot)
                {
                    float rr = (d - 2) / 2;
                    br = VGrad(cx, cy - rr, rr * 2, down ? ARGB(200, 0xB5, 0xCC, 0xE8) : ARGB(190, 0xFF, 0xFF, 0xFF),
                               down ? ARGB(200, 0xD8, 0xE5, 0xF4) : ARGB(190, 0xC8, 0xDC, 0xF2));
                    FillRRect(g, r->left + 1.f, r->top + 1.f, (float)(r->right - r->left - 2), (float)(r->bottom - r->top - 2), rr * 0.8f, br, NULL);
                    GdipDeleteBrush(br);
                }
                DrawPillIcon(g, ico, cx - (i == PI_ZOOM ? d * 0.0f : 0.f), cy, d * (i == PI_PLAY ? 1.05f : 0.9f), en);
                continue;
            }
            if (i == PI_PLAY)
            {
                /* the dominant, accent-filled centre button */
                br = !en ? VGrad(cx, cy - d / 2, d, ARGB(255, 0xD3, 0xD9, 0xE0), ARGB(255, 0xA9, 0xB2, 0xBD))
                    : down ? VGrad(cx, cy - d / 2, d, ARGB(255, 0x3F, 0x86, 0xD8), ARGB(255, 0x14, 0x4C, 0x9A))
                          : VGrad(cx, cy - d / 2, d, hot ? ARGB(255, 0x9A, 0xD2, 0xFB) : ARGB(255, 0x7C, 0xC0, 0xF7),
                                  hot ? ARGB(255, 0x2D, 0x78, 0xD0) : ARGB(255, 0x1F, 0x5F, 0xB8));
                pen = MakePen(ARGB(255, 0x17, 0x3F, 0x7A), 1.2f);
                Disc(g, cx, cy, d, br, pen);
                GdipDeleteBrush(br);
                GdipDeletePen(pen);
                br = VGrad(cx, cy - d / 2, d / 2, ARGB(120, 255, 255, 255), ARGB(0, 255, 255, 255));
                GdipFillEllipse(g, br, cx - d * 0.42f, cy - d * 0.46f, d * 0.84f, d * 0.5f);
                GdipDeleteBrush(br);
            }
            else if (i == PI_PREV || i == PI_NEXT)
            {
                br = down ? VGrad(cx, cy - d / 2, d, ARGB(255, 0xC8, 0xD3, 0xE0), ARGB(255, 0xEE, 0xF2, 0xF7))
                          : VGrad(cx, cy - d / 2, d, hot ? ARGB(255, 0xFF, 0xFF, 0xFF) : ARGB(255, 0xF8, 0xFA, 0xFC),
                                  hot ? ARGB(255, 0xBF, 0xD6, 0xF0) : ARGB(255, 0xD0, 0xD9, 0xE4));
                pen = MakePen(ARGB(255, 0x7F, 0x8C, 0x9C), 1.f);
                Disc(g, cx, cy, d - 2, br, pen);
                GdipDeleteBrush(br);
                GdipDeletePen(pen);
            }
            else if (hot)
            {
                float rr = (d - 2) / 2;
                br = VGrad(cx, cy - rr, rr * 2, down ? ARGB(200, 0xB5, 0xCC, 0xE8) : ARGB(190, 0xFF, 0xFF, 0xFF),
                           down ? ARGB(200, 0xD8, 0xE5, 0xF4) : ARGB(190, 0xC8, 0xDC, 0xF2));
                pen = MakePen(ARGB(200, 0x7F, 0x9C, 0xC0), 1.f);
                FillRRect(g, r->left + 1.f, r->top + 1.f, (float)(r->right - r->left - 2), (float)(r->bottom - r->top - 2), rr * 0.8f, br, pen);
                GdipDeleteBrush(br);
                GdipDeletePen(pen);
            }
            DrawGlyph(g, i, cx - (i == PI_ZOOM ? d * 0.18f : 0.f), cy, d * (i == PI_PLAY ? 0.95f : 0.62f), en, fit, hot);
        }
    }
    if (g)
        GdipDeleteGraphics(g);
    if (bmp)
        GdipDisposeImage((GpImage *)bmp);

    GetWindowRect(p->pill, &wr);
    ptDst.x = wr.left;
    ptDst.y = wr.top;
    if (!p->plain)
    {
        if (!UpdateLayeredWindow(p->pill, hdcScreen, &ptDst, &p->size, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA))
            p->needPlain = TRUE;
    }
    SelectObject(hdcMem, hbmOld);
done:
    if (hbm)
    {
        if (p->hbmSnap)
            DeleteObject(p->hbmSnap);
        p->hbmSnap = hbm;
    }
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (p->plain)
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, p->size.cx + 1, p->size.cy + 1, p->size.cy, p->size.cy);
        SetWindowRgn(p->pill, rgn, FALSE);
        InvalidateRect(p->pill, NULL, FALSE);
    }
    if (p->needPlain)
    {
        p->needPlain = FALSE;
        SwitchToPlain(p);
    }
}

static int
PillHit(VPILL *p, int x, int y)
{
    POINT pt = { x, y };
    int i;
    for (i = 0; i < PI_COUNT; i++)
    {
        if (PtInRect(&p->itemRc[i], pt))
            return i;
    }
    return -1;
}

static void
PillTipSync(VPILL *p)
{
    static const UINT ids[PI_COUNT] =
    {
        IDS_TOOLTIP_DISPLAYSIZE, IDS_TOOLTIP_FIT_TOGGLE, IDS_TOOLTIP_PREV_PIC, IDS_TOOLTIP_SLIDE_SHOW,
        IDS_TOOLTIP_NEXT_PIC, IDS_TOOLTIP_ROT_COUNCW, IDS_TOOLTIP_ROT_CLOCKW, IDS_TOOLTIP_DELETE
    };
    TOOLINFOW ti;
    int i;
    if (!p->tip)
        return;
    for (i = 0; i < PI_COUNT; i++)
    {
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd = p->pill;
        ti.uId = (UINT_PTR)(i + 1);
        SendMessageW(p->tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hinst = p->inst;
        ti.lpszText = MAKEINTRESOURCEW(ids[i]);
        ti.rect = p->itemRc[i];
        SendMessageW(p->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

/* ---- zoom slider popup ---- */

static float
ZoomToT(UINT z)
{
    double lo = log((double)VPILL_ZOOM_MIN), hi = log((double)VPILL_ZOOM_MAX);
    double v = log((double)(z ? z : 1));
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (float)((v - lo) / (hi - lo));
}

static UINT
TToZoom(float t)
{
    double lo = log((double)VPILL_ZOOM_MIN), hi = log((double)VPILL_ZOOM_MAX);
    UINT z;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    z = (UINT)(exp(lo + (hi - lo) * t) + 0.5);
    /* snap to 100 % when close: the stop users want most */
    if (z > 92 && z < 108)
        z = 100;
    return z;
}

static void
SliderGeometry(VPILL *p, HWND hwnd, RECT *track)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    SetRect(track, rc.right / 2 - 1, Sc(p, 26), rc.right / 2 + 2, rc.bottom - Sc(p, 12));
}

static LRESULT CALLBACK
SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VPILL *p = s_active;
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc, track;
            WCHAR t[16];
            HBRUSH br;
            HGDIOBJ old;
            int y;
            if (p)
            {
                GetClientRect(hwnd, &rc);
                br = CreateSolidBrush(RGB(0xF4, 0xF7, 0xFB));
                FillRect(hdc, &rc, br);
                DeleteObject(br);
                {
                    HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, RGB(0x9A, 0xA6, 0xB4)));
                    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    RoundRect(hdc, 0, 0, rc.right, rc.bottom, Sc(p, 14), Sc(p, 14));
                    SelectObject(hdc, ob);
                    DeleteObject(SelectObject(hdc, op));
                }
                SliderGeometry(p, hwnd, &track);
                br = CreateSolidBrush(RGB(0x9A, 0xA6, 0xB4));
                FillRect(hdc, &track, br);
                DeleteObject(br);
                /* 100 % tick */
                y = track.bottom - (int)(ZoomToT(100) * (track.bottom - track.top));
                br = CreateSolidBrush(RGB(0x6C, 0x77, 0x84));
                {
                    RECT tk = { track.left - Sc(p, 5), y, track.right + Sc(p, 5), y + 1 };
                    FillRect(hdc, &tk, br);
                }
                DeleteObject(br);
                /* thumb */
                y = track.bottom - (int)(ZoomToT(p->host.GetZoom(p->host.ctx)) * (track.bottom - track.top));
                {
                    RECT th = { track.left - Sc(p, 7), y - Sc(p, 5), track.right + Sc(p, 7), y + Sc(p, 5) };
                    br = CreateSolidBrush(RGB(0x1F, 0x5F, 0xB8));
                    FillRect(hdc, &th, br);
                    DeleteObject(br);
                }
                StringCchPrintfW(t, _countof(t), L"%u%%", p->host.GetZoom(p->host.ctx));
                old = SelectObject(hdc, p->font);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(0x3B, 0x46, 0x50));
                rc.bottom = Sc(p, 24);
                DrawTextW(hdc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, old);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
            if (p && ((msg == WM_LBUTTONDOWN) || (wp & MK_LBUTTON)))
            {
                RECT rc, track;
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                GetClientRect(hwnd, &rc);
                if (!PtInRect(&rc, pt))
                {
                    if (msg == WM_LBUTTONDOWN)
                        DestroyWindow(hwnd);
                    return 0;
                }
                SliderGeometry(p, hwnd, &track);
                p->host.SetZoom(p->host.ctx, TToZoom((float)(track.bottom - pt.y) / (float)(track.bottom - track.top)));
                InvalidateRect(hwnd, NULL, FALSE);
                VPill_Refresh(p);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (p)
            {
                float t = ZoomToT(p->host.GetZoom(p->host.ctx)) + ((short)HIWORD(wp) > 0 ? 0.04f : -0.04f);
                p->host.SetZoom(p->host.ctx, TToZoom(t));
                InvalidateRect(hwnd, NULL, FALSE);
                VPill_Refresh(p);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd)
                DestroyWindow(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN)
                DestroyWindow(hwnd);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DESTROY:
            if (p && p->slider == hwnd)
                p->slider = NULL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void
ToggleSlider(VPILL *p)
{
    RECT zr, wr;
    int w = Sc(p, 52), h = Sc(p, 190), x, y;
    POINT pt;
    if (p->slider)
    {
        DestroyWindow(p->slider);
        return;
    }
    GetWindowRect(p->pill, &wr);
    zr = p->itemRc[PI_ZOOM];
    pt.x = wr.left + (zr.left + zr.right) / 2;
    pt.y = wr.top + zr.top;
    x = pt.x - w / 2;
    y = pt.y - h - Sc(p, 4);
    p->slider = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, SLIDER_CLASS, NULL,
                                WS_POPUP, x, y, w, h, p->main, NULL, p->inst, NULL);
    if (p->slider)
    {
        SetWindowRgn(p->slider, CreateRoundRectRgn(0, 0, w + 1, h + 1, Sc(p, 14), Sc(p, 14)), FALSE);
        ShowWindow(p->slider, SW_SHOWNA);
        SetCapture(p->slider);
    }
}

/* ---- pill window ---- */

static LRESULT CALLBACK
PillProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VPILL *p = (VPILL *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p)
        return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg)
    {
        case WM_NCHITTEST:
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT wr;
            GetWindowRect(hwnd, &wr);
            pt.x -= wr.left;
            pt.y -= wr.top;
            return PtInRect(&p->capsule, pt) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (p->plain && p->hbmSnap)
            {
                HDC mem = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(mem, p->hbmSnap);
                BitBlt(hdc, 0, 0, p->size.cx, p->size.cy, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
#ifdef VPILL_TESTHOOK
        case WM_APP + 0x79: /* test builds: force the hot item (a real hover is cancelled by WM_MOUSELEAVE) */
            p->hotPill = (int)wp;
            p->downPill = lp ? (int)wp : -1;
            RenderPill(p);
            return 0;
        case WM_APP + 0x78:
            /* Test builds: centre of pill item wp, as MAKELONG(x, y) in pill client pixels */
            if ((UINT)wp < PI_COUNT)
                return MAKELONG((p->itemRc[wp].left + p->itemRc[wp].right) / 2, (p->itemRc[wp].top + p->itemRc[wp].bottom) / 2);
            return -1;
        case WM_APP + 0x77:
        {
            /* Test builds: dump the last rendering (premultiplied BGRA) to %TEMP%pill.raw
             * as w, h, then the pixels, for screenshot compositing. */
            WCHAR path[MAX_PATH];
            HANDLE hf;
            DWORD wr, hdr[2];
            BITMAP bm;
            if (p->hbmSnap && GetObjectW(p->hbmSnap, sizeof(bm), &bm) && bm.bmBits &&
                GetTempPathW(MAX_PATH, path) && SUCCEEDED(StringCchCatW(path, MAX_PATH, L"vpill.raw")))
            {
                hf = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE)
                {
                    hdr[0] = (DWORD)bm.bmWidth;
                    hdr[1] = (DWORD)bm.bmHeight;
                    WriteFile(hf, hdr, sizeof(hdr), &wr, NULL);
                    WriteFile(hf, bm.bmBits, (DWORD)(bm.bmWidth * bm.bmHeight * 4), &wr, NULL);
                    CloseHandle(hf);
                    return 1;
                }
            }
            return 0;
        }
#endif
        case WM_MOUSEMOVE:
        {
            int hit = PillHit(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!p->tracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                p->tracking = TRUE;
            }
            if (hit != p->hotPill)
            {
                p->hotPill = hit;
                RenderPill(p);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            p->tracking = FALSE;
            if (p->hotPill != -1 || p->downPill != -1)
            {
                p->hotPill = p->downPill = -1;
                RenderPill(p);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        {
            int hit = PillHit(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0 && ItemEnabled(p, hit))
            {
                p->downPill = hit;
                SetCapture(hwnd);
                RenderPill(p);
            }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            int hit = PillHit(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            int down = p->downPill;
            p->downPill = -1;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (down >= 0 && down == hit)
            {
                if (down == PI_ZOOM)
                    ToggleSlider(p);
                else if (down == PI_FIT)
                    p->host.Execute(p->host.ctx, p->host.IsFit && p->host.IsFit(p->host.ctx) ? IDC_REAL_SIZE : IDC_BEST_FIT);
                else
                    p->host.Execute(p->host.ctx, s_ItemCmd[down]);
            }
            RenderPill(p);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (p->downPill != -1)
            {
                p->downPill = -1;
                RenderPill(p);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- menu strip ---- */

static void
StripLayout(VPILL *p)
{
    HDC hdc = GetDC(p->strip);
    HGDIOBJ old = SelectObject(hdc, p->font);
    int x = Sc(p, 6), h = (int)VPill_StripHeight(p), i;
    for (i = 0; i < 5; i++)
    {
        WCHAR t[48];
        SIZE sz;
        int w;
        Text(p, p->items[i].idsLabel, t, _countof(t));
        GetTextExtentPoint32W(hdc, t, lstrlenW(t), &sz);
        w = sz.cx + Sc(p, 20) + (p->items[i].dropdown ? Sc(p, 12) : 0);
        SetRect(&p->items[i].rc, x, Sc(p, 3), x + w, h - Sc(p, 3));
        x += w + Sc(p, 2);
    }
    SelectObject(hdc, old);
    ReleaseDC(p->strip, hdc);
}

static void
StripPaint(VPILL *p, HDC hdc)
{
    RECT rc;
    TRIVERTEX tv[2];
    GRADIENT_RECT gr = { 0, 1 };
    HGDIOBJ old;
    HBRUSH br;
    int i;

    GetClientRect(p->strip, &rc);
    tv[0].x = 0; tv[0].y = 0; tv[0].Red = 0xFF00; tv[0].Green = 0xFF00; tv[0].Blue = 0xFF00; tv[0].Alpha = 0;
    tv[1].x = rc.right; tv[1].y = rc.bottom; tv[1].Red = 0xE600; tv[1].Green = 0xEA00; tv[1].Blue = 0xF000; tv[1].Alpha = 0;
    GradientFill(hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    br = CreateSolidBrush(RGB(0xC5, 0xCC, 0xD6));
    {
        RECT ln = { 0, rc.bottom - 1, rc.right, rc.bottom };
        FillRect(hdc, &ln, br);
    }
    DeleteObject(br);

    old = SelectObject(hdc, p->font);
    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < 5; i++)
    {
        const StripItem *it = &p->items[i];
        WCHAR t[48];
        RECT tr = it->rc;
        BOOL hot = (i == p->hotStrip), open = (i == p->openStrip);
        if (hot || open)
        {
            HBRUSH f = CreateSolidBrush(open ? RGB(0xD3, 0xE5, 0xF8) : RGB(0xE8, 0xF1, 0xFB));
            HBRUSH b = CreateSolidBrush(RGB(0xA9, 0xC1, 0xDE));
            FillRect(hdc, &it->rc, f);
            FrameRect(hdc, &it->rc, b);
            DeleteObject(f);
            DeleteObject(b);
        }
        Text(p, it->idsLabel, t, _countof(t));
        SetTextColor(hdc, RGB(0x14, 0x18, 0x1F));
        tr.left += Sc(p, 10);
        tr.right -= it->dropdown ? Sc(p, 14) : Sc(p, 10);
        DrawTextW(hdc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (it->dropdown)
        {
            int cx = it->rc.right - Sc(p, 12), cy = (it->rc.top + it->rc.bottom) / 2, a = Sc(p, 3);
            POINT tri[3] = { { cx - a, cy - 1 }, { cx + a + 1, cy - 1 }, { cx, cy + a } };
            HBRUSH tb = CreateSolidBrush(RGB(0x3B, 0x46, 0x50));
            HGDIOBJ ob = SelectObject(hdc, tb);
            HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
            Polygon(hdc, tri, 3);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(tb);
        }
    }
    SelectObject(hdc, old);
}

static void
AddItem(HMENU m, VPILL *p, UINT cmd, UINT ids, BOOL enabled)
{
    WCHAR t[96];
    Text(p, ids, t, _countof(t));
    AppendMenuW(m, MF_STRING | (enabled ? 0 : MF_GRAYED), cmd, t);
}

static void
StripDropdown(VPILL *p, int i)
{
    HMENU m = CreatePopupMenu();
    BOOL has = p->host.HasImage ? p->host.HasImage(p->host.ctx) : TRUE;
    POINT pt;
    UINT cmd;

    switch (i)
    {
        case 0: /* File */
            AddItem(m, p, VRB_CMD_OPEN, IDS_ITEM_OPEN, TRUE);
            AddItem(m, p, IDC_SAVEAS, IDS_ITEM_SAVEAS, has);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AddItem(m, p, IDC_MENU_COPY, IDS_ITEM_COPY, has);
            AddItem(m, p, IDC_MENU_COPYPATH, IDS_ITEM_COPYPATH, has);
            AddItem(m, p, IDC_DELETE, IDS_ITEM_DELETE, has);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AddItem(m, p, VRB_CMD_WALLPAPER, IDS_ITEM_WALLPAPER, has);
            AddItem(m, p, IDC_MENU_PROPS, IDS_ITEM_PROPS, has);
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AddItem(m, p, VRB_CMD_EXIT, IDS_ITEM_EXIT, TRUE);
            break;
        case 1: /* Print */
            AddItem(m, p, IDC_PRINT, IDS_ITEM_PRINT, has && p->host.IsEnabled(p->host.ctx, IDC_PRINT));
            break;
        case 3: /* Burn */
            AddItem(m, p, IDC_MENU_BURN, IDS_ITEM_BURN, has);
            break;
        case 4: /* Open */
            AddItem(m, p, IDC_MODIFY, IDS_ITEM_EDIT, has && p->host.IsEnabled(p->host.ctx, IDC_MODIFY));
            AddItem(m, p, IDC_MENU_OPENWITH, IDS_ITEM_OPENWITH, has);
            break;
        default:
            DestroyMenu(m);
            return;
    }
    pt.x = p->items[i].rc.left;
    pt.y = p->items[i].rc.bottom;
    ClientToScreen(p->strip, &pt);
    p->openStrip = i;
    InvalidateRect(p->strip, NULL, FALSE);
    UpdateWindow(p->strip);
    cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON, pt.x, pt.y, 0, p->strip, NULL);
    DestroyMenu(m);
    p->openStrip = -1;
    InvalidateRect(p->strip, NULL, FALSE);
    if (cmd)
        p->host.Execute(p->host.ctx, cmd);
}

static int
StripHit(VPILL *p, int x, int y)
{
    POINT pt = { x, y };
    int i;
    for (i = 0; i < 5; i++)
    {
        if (PtInRect(&p->items[i].rc, pt))
            return i;
    }
    return -1;
}

static LRESULT CALLBACK
StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VPILL *p = (VPILL *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p)
        return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            StripPaint(p, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            int hit = StripHit(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            if (hit != p->hotStrip)
            {
                p->hotStrip = hit;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            p->hotStrip = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        {
            int hit = StripHit(p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0)
            {
                if (p->items[hit].dropdown)
                    StripDropdown(p, hit);
                else
                    p->host.Execute(p->host.ctx, hit == 2 ? IDC_MENU_EMAIL : 0);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- public ---- */

static void
RegisterClasses(HINSTANCE inst)
{
    WNDCLASSW wc;
    if (s_atoms[0])
        return;
    ZeroMemory(&wc, sizeof(wc));
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = STRIP_CLASS;
    wc.lpfnWndProc = StripProc;
    s_atoms[0] = RegisterClassW(&wc);
    wc.lpszClassName = PILL_CLASS;
    wc.lpfnWndProc = PillProc;
    wc.style = CS_DBLCLKS;
    s_atoms[1] = RegisterClassW(&wc);
    wc.lpszClassName = SLIDER_CLASS;
    wc.lpfnWndProc = SliderProc;
    wc.style = CS_DROPSHADOW;
    s_atoms[2] = RegisterClassW(&wc);
}

static void
MakeFont(VPILL *p)
{
    NONCLIENTMETRICSW ncm;
    if (p->font)
        DeleteObject(p->font);
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        ncm.lfMenuFont.lfHeight = -Sc(p, 12);
        ncm.lfMenuFont.lfWidth = 0;
        p->font = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    if (!p->font)
        p->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* HKCU\Software\Leeway\PhotoViewer\PillWindow = "plain" forces the non-layered pill. */
static BOOL
PillForcedPlain(void)
{
    HKEY hKey;
    WCHAR buf[16];
    DWORD cb = sizeof(buf), type = 0;
    BOOL plain = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Leeway\\PhotoViewer", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        buf[0] = 0;
        if (RegQueryValueExW(hKey, L"PillWindow", NULL, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ)
            plain = !lstrcmpiW(buf, L"plain");
        RegCloseKey(hKey);
    }
    return plain;
}

static HWND
CreatePillWindow(VPILL *p)
{
    HWND h = CreateWindowExW((p->plain ? 0 : WS_EX_LAYERED) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PILL_CLASS, NULL,
                             WS_POPUP, 0, 0, 10, 10, p->main, NULL, p->inst, NULL);
    if (h)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
    return h;
}

VPILL *
VPill_CreateOverlay(HWND owner, HINSTANCE inst, const VPILL_HOST *host)
{
    VPILL *p = (VPILL *)LocalAlloc(LPTR, sizeof(*p));

    if (!p)
        return NULL;
    p->main = owner;
    p->inst = inst;
    p->host = *host;
    p->dpi = GetDpiFor(owner);
    p->hotStrip = p->openStrip = -1;
    p->hotPill = p->downPill = -1;
    p->overlay = TRUE;
    p->plain = PillForcedPlain();
    p->margin = p->plain ? 0 : Sc(p, 12);
    RegisterClasses(inst);
    MakeFont(p);
    p->pill = CreatePillWindow(p);
    if (!p->pill)
    {
        VPill_Destroy(p);
        return NULL;
    }
    p->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             0, 0, 0, 0, p->pill, NULL, inst, NULL);
    LoadIcons(p);
    PillMetrics(p);
    PillTipSync(p);
    return p;
}

BOOL
VPill_IsCursorOver(VPILL *p)
{
    POINT pt;
    RECT rc;
    return p && p->pill && IsWindowVisible(p->pill) && GetCursorPos(&pt) && GetWindowRect(p->pill, &rc) &&
           PtInRect(&rc, pt);
}

void
VPill_ShowOverlay(VPILL *p, BOOL show, int cx, int cy)
{
    if (!p || !p->pill)
        return;
    if (!show)
    {
        ShowWindow(p->pill, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(p->pill) || cx != p->lastCx || cy != p->lastCy)
        VPill_Layout(p, cx, cy);
    if (!IsWindowVisible(p->pill))
        ShowWindow(p->pill, SW_SHOWNOACTIVATE);
}

VPILL *
VPill_Create(HWND main, HINSTANCE inst, const VPILL_HOST *host)
{
    VPILL *p = (VPILL *)LocalAlloc(LPTR, sizeof(*p));
    static const UINT labels[5] = { IDS_MENU_FILE, IDS_MENU_PRINT, IDS_MENU_EMAIL, IDS_MENU_BURN, IDS_MENU_OPEN };
    static const BOOL drops[5] = { TRUE, TRUE, FALSE, TRUE, TRUE };
    int i;

    if (!p)
        return NULL;
    p->main = main;
    p->inst = inst;
    p->host = *host;
    p->dpi = GetDpiFor(main);
    p->hotStrip = p->openStrip = -1;
    p->hotPill = p->downPill = -1;
    for (i = 0; i < 5; i++)
    {
        p->items[i].idsLabel = labels[i];
        p->items[i].dropdown = drops[i];
    }
    RegisterClasses(inst);
    MakeFont(p);

    p->strip = CreateWindowExW(0, STRIP_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, main, NULL, inst, NULL);
    p->plain = PillForcedPlain();
    p->margin = p->plain ? 0 : Sc(p, 12);
    p->pill = CreatePillWindow(p);
    if (!p->strip || !p->pill)
    {
        VPill_Destroy(p);
        return NULL;
    }
    SetWindowLongPtrW(p->strip, GWLP_USERDATA, (LONG_PTR)p);
    SetWindowLongPtrW(p->pill, GWLP_USERDATA, (LONG_PTR)p);
    p->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             0, 0, 0, 0, p->pill, NULL, inst, NULL);
    StripLayout(p);
    LoadIcons(p);
    PillMetrics(p);
    PillTipSync(p);
    s_active = p;
    return p;
}

void
VPill_Destroy(VPILL *p)
{
    if (!p)
        return;
    if (s_active == p)
        s_active = NULL;
    if (p->slider)
        DestroyWindow(p->slider);
    if (p->tip)
        DestroyWindow(p->tip);
    if (p->pill)
        DestroyWindow(p->pill);
    if (p->strip)
        DestroyWindow(p->strip);
    if (p->font && p->font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(p->font);
    if (p->hbmSnap)
        DeleteObject(p->hbmSnap);
    {
        int k;
        for (k = 0; k < PI_COUNT + 1; k++)
            if (p->icon[k])
                GdipDisposeImage((GpImage *)p->icon[k]);
    }
    LocalFree(p);
}

UINT
VPill_StripHeight(VPILL *p)
{
    return p ? (UINT)Sc(p, 28) : 0;
}

HWND
VPill_PillWindow(VPILL *p)
{
    return p ? p->pill : NULL;
}

HWND
VPill_StripWindow(VPILL *p)
{
    return p ? p->strip : NULL;
}

void
VPill_Layout(VPILL *p, int cx, int cy)
{
    POINT pt;
    int sh, x, y;
    UINT flags = SWP_NOACTIVATE;
    if (!p)
        return;
    p->lastCx = cx;
    p->lastCy = cy;
    sh = p->overlay ? 0 : (int)VPill_StripHeight(p);
    if (p->strip)
    {
        MoveWindow(p->strip, 0, 0, cx, sh, TRUE);
        StripLayout(p);
    }
    if (p->overlay)
        sh = 0;

    PillMetrics(p);
    /* bottom-centred over the image area, 14 px above its lower edge */
    x = (cx - (p->capsule.right - p->capsule.left)) / 2 - p->capsule.left;
    y = cy - (p->capsule.bottom) - Sc(p, 14);
    if (y < sh)
        y = sh;
    pt.x = x;
    pt.y = y;
    ClientToScreen(p->main, &pt);
    p->origin = pt;
    if (!p->overlay && !IsWindowVisible(p->pill) && IsWindowVisible(p->main) && !IsIconic(p->main))
        flags |= SWP_SHOWWINDOW;
    SetWindowPos(p->pill, HWND_TOP, pt.x, pt.y, p->size.cx, p->size.cy, flags);
    PillTipSync(p);
    RenderPill(p);
}

void
VPill_Refresh(VPILL *p)
{
    if (!p || !p->pill)
        return;
    RenderPill(p);
    InvalidateRect(p->strip, NULL, FALSE);
}

void
VPill_OnDpiChanged(VPILL *p, UINT dpi)
{
    if (!p)
        return;
    p->dpi = dpi ? dpi : 96;
    if (!p->plain)
        p->margin = Sc(p, 12);
    MakeFont(p);
}
