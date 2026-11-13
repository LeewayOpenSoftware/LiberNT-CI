/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that DirectWrite, GDI and GDI+ give Segoe UI text the width GDI draws it with
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#include <apitest.h>
#include <wingdi.h>
#include <winuser.h>
#include <dwrite.h>

typedef struct _GP_STARTUP_INPUT
{
    UINT32 GdiplusVersion;
    PVOID DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GP_STARTUP_INPUT;

typedef struct _GP_RECTF
{
    FLOAT X;
    FLOAT Y;
    FLOAT Width;
    FLOAT Height;
} GP_RECTF;

typedef INT (WINAPI *PGDIPLUS_STARTUP)(ULONG_PTR *, const GP_STARTUP_INPUT *, PVOID);
typedef VOID (WINAPI *PGDIPLUS_SHUTDOWN)(ULONG_PTR);
typedef INT (WINAPI *PGDIP_CREATE_FONT_FAMILY_FROM_NAME)(const WCHAR *, PVOID, PVOID *);
typedef INT (WINAPI *PGDIP_GET_FAMILY_NAME)(PVOID, WCHAR *, LANGID);
typedef INT (WINAPI *PGDIP_DELETE_FONT_FAMILY)(PVOID);
typedef INT (WINAPI *PGDIP_CREATE_FONT)(PVOID, FLOAT, INT, INT, PVOID *);
typedef INT (WINAPI *PGDIP_DELETE_FONT)(PVOID);
typedef INT (WINAPI *PGDIP_CREATE_FROM_HDC)(HDC, PVOID *);
typedef INT (WINAPI *PGDIP_DELETE_GRAPHICS)(PVOID);
typedef INT (WINAPI *PGDIP_MEASURE_STRING)(PVOID, const WCHAR *, INT, PVOID, const GP_RECTF *, PVOID,
                                           GP_RECTF *, INT *, INT *);

#define GP_UNIT_PIXEL 2
#define CANVAS_WIDTH 1024
#define CANVAS_HEIGHT 96

typedef struct _METRICS_CASE
{
    const WCHAR *Text;
    const WCHAR *GdiFace;
    const WCHAR *Family;
    DWRITE_FONT_WEIGHT Weight;
} METRICS_CASE;

static const METRICS_CASE Cases[] =
{
    { L"Se produjo un error", L"Segoe UI Semibold", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD },
    { L"Se produjo un error", L"Segoe UI", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL },
    { L"Tuvimos un problema al iniciar la instalaci\x00f3n", L"Segoe UI", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL },
    { L"Tuvimos un problema al iniciar la instalaci\x00f3n", L"Segoe UI Semilight", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_LIGHT },
};

static IDWriteFactory *Factory;
static HMODULE hGdiPlus;
static ULONG_PTR GpToken;
static PGDIP_CREATE_FONT_FAMILY_FROM_NAME pGdipCreateFontFamilyFromName;
static PGDIP_GET_FAMILY_NAME pGdipGetFamilyName;
static PGDIP_DELETE_FONT_FAMILY pGdipDeleteFontFamily;
static PGDIP_CREATE_FONT pGdipCreateFont;
static PGDIP_DELETE_FONT pGdipDeleteFont;
static PGDIP_CREATE_FROM_HDC pGdipCreateFromHDC;
static PGDIP_DELETE_GRAPHICS pGdipDeleteGraphics;
static PGDIP_MEASURE_STRING pGdipMeasureString;

static
BOOL
GdiWidths(
    _In_ HDC hdc,
    _In_ PULONG Bits,
    _In_ const METRICS_CASE *Case,
    _In_ INT Pixels,
    _Out_ PLONG Extent,
    _Out_ PLONG Ink,
    _Out_writes_(LF_FACESIZE) PWSTR Face)
{
    HFONT hFont, hOld;
    SIZE Size;
    RECT Rect = { 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT };
    INT x, y;

    *Extent = 0;
    *Ink = 0;
    Face[0] = UNICODE_NULL;

    hFont = CreateFontW(-Pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, Case->GdiFace);
    if (hFont == NULL)
        return FALSE;

    hOld = SelectObject(hdc, hFont);
    GetTextFaceW(hdc, LF_FACESIZE, Face);
    GetTextExtentPoint32W(hdc, Case->Text, lstrlenW(Case->Text), &Size);
    *Extent = Size.cx;

    FillRect(hdc, &Rect, GetStockObject(WHITE_BRUSH));
    SetTextColor(hdc, RGB(0, 0, 0));
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, 8, 8, Case->Text, lstrlenW(Case->Text));
    GdiFlush();

    for (y = 0; y < CANVAS_HEIGHT; y++)
    {
        for (x = CANVAS_WIDTH - 1; x > *Ink + 7; x--)
        {
            if ((Bits[y * CANVAS_WIDTH + x] & 0x00FFFFFF) != 0x00FFFFFF)
            {
                *Ink = x - 7;
                break;
            }
        }
    }

    SelectObject(hdc, hOld);
    DeleteObject(hFont);
    return TRUE;
}

static
FLOAT
DWriteWidth(
    _In_ const WCHAR *Family,
    _In_ DWRITE_FONT_WEIGHT Weight,
    _In_ const WCHAR *Text,
    _In_ INT Pixels)
{
    IDWriteTextFormat *Format = NULL;
    IDWriteTextLayout *Layout = NULL;
    DWRITE_TEXT_METRICS Metrics;
    FLOAT Width = -1.0f;
    HRESULT hr;

    hr = IDWriteFactory_CreateTextFormat(Factory, Family, NULL, Weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, (FLOAT)Pixels, L"es-es", &Format);
    if (FAILED(hr))
        return Width;

    hr = IDWriteFactory_CreateTextLayout(Factory, Text, lstrlenW(Text), Format, 10000.0f, 1000.0f, &Layout);
    if (SUCCEEDED(hr) && SUCCEEDED(IDWriteTextLayout_GetMetrics(Layout, &Metrics)))
        Width = Metrics.widthIncludingTrailingWhitespace;

    if (Layout != NULL)
        IDWriteTextLayout_Release(Layout);
    IDWriteTextFormat_Release(Format);
    return Width;
}

static
FLOAT
GdiPlusWidth(
    _In_ HDC hdc,
    _In_ const WCHAR *Family,
    _In_ const WCHAR *Text,
    _In_ INT Pixels,
    _Out_writes_(LF_FACESIZE) PWSTR Name)
{
    PVOID FontFamily = NULL, Font = NULL, Graphics = NULL;
    GP_RECTF Layout = { 0.0f, 0.0f, 0.0f, 0.0f }, Bounds;
    FLOAT Width = -1.0f;
    INT Status;

    Name[0] = UNICODE_NULL;
    if (pGdipMeasureString == NULL)
        return Width;

    Status = pGdipCreateFontFamilyFromName(Family, NULL, &FontFamily);
    if (Status != 0)
    {
        _snwprintf(Name, LF_FACESIZE, L"status %d", Status);
        return Width;
    }

    pGdipGetFamilyName(FontFamily, Name, 0);
    if (pGdipCreateFont(FontFamily, (FLOAT)Pixels, 0, GP_UNIT_PIXEL, &Font) == 0 &&
        pGdipCreateFromHDC(hdc, &Graphics) == 0 &&
        pGdipMeasureString(Graphics, Text, -1, Font, &Layout, NULL, &Bounds, NULL, NULL) == 0)
    {
        Width = Bounds.Width;
    }

    if (Graphics != NULL)
        pGdipDeleteGraphics(Graphics);
    if (Font != NULL)
        pGdipDeleteFont(Font);
    pGdipDeleteFontFamily(FontFamily);
    return Width;
}

static
VOID
LoadGdiPlus(VOID)
{
    GP_STARTUP_INPUT Input = { 1, NULL, FALSE, FALSE };
    PGDIPLUS_STARTUP pGdiplusStartup;

    hGdiPlus = LoadLibraryW(L"gdiplus.dll");
    if (hGdiPlus == NULL)
        return;

    pGdiplusStartup = (PGDIPLUS_STARTUP)GetProcAddress(hGdiPlus, "GdiplusStartup");
    if (pGdiplusStartup == NULL || pGdiplusStartup(&GpToken, &Input, NULL) != 0)
        return;

    pGdipCreateFontFamilyFromName = (PVOID)GetProcAddress(hGdiPlus, "GdipCreateFontFamilyFromName");
    pGdipGetFamilyName = (PVOID)GetProcAddress(hGdiPlus, "GdipGetFamilyName");
    pGdipDeleteFontFamily = (PVOID)GetProcAddress(hGdiPlus, "GdipDeleteFontFamily");
    pGdipCreateFont = (PVOID)GetProcAddress(hGdiPlus, "GdipCreateFont");
    pGdipDeleteFont = (PVOID)GetProcAddress(hGdiPlus, "GdipDeleteFont");
    pGdipCreateFromHDC = (PVOID)GetProcAddress(hGdiPlus, "GdipCreateFromHDC");
    pGdipDeleteGraphics = (PVOID)GetProcAddress(hGdiPlus, "GdipDeleteGraphics");
    pGdipMeasureString = (PVOID)GetProcAddress(hGdiPlus, "GdipMeasureString");
}

static
VOID
TestFamilies(VOID)
{
    static const WCHAR *Families[] = { L"Segoe UI", L"Segoe UI Semibold", L"Segoe UI Semilight", L"Selawik" };
    IDWriteFontCollection *Collection = NULL;
    UINT32 Index;
    BOOL Exists;
    ULONG i;

    if (FAILED(IDWriteFactory_GetSystemFontCollection(Factory, &Collection, FALSE)))
    {
        ok(FALSE, "GetSystemFontCollection failed\n");
        return;
    }

    for (i = 0; i < ARRAYSIZE(Families); i++)
    {
        Exists = FALSE;
        IDWriteFontCollection_FindFamilyName(Collection, Families[i], &Index, &Exists);
        ok(Exists, "DirectWrite system collection lacks '%ls'\n", Families[i]);
    }

    IDWriteFontCollection_Release(Collection);
}

START_TEST(SegoeUiMetrics)
{
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), CANVAS_WIDTH, -CANVAS_HEIGHT, 1, 32, BI_RGB } };
    WCHAR Face[LF_FACESIZE], GpName[LF_FACESIZE];
    HBITMAP hBitmap, hOld;
    PULONG Bits = NULL;
    LONG Extent, Ink;
    FLOAT DWrite, DWriteFace, DWriteSelawik, GdiPlus;
    HDC hdc;
    ULONG i;
    INT Pixels;
    HRESULT hr;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&Factory);
    ok(SUCCEEDED(hr), "DWriteCreateFactory failed 0x%lx\n", hr);
    if (FAILED(hr))
        return;

    TestFamilies();
    LoadGdiPlus();

    hdc = CreateCompatibleDC(NULL);
    hBitmap = CreateDIBSection(hdc, &Info, DIB_RGB_COLORS, (PVOID *)&Bits, NULL, 0);
    ok(hBitmap != NULL, "CreateDIBSection failed %lu\n", GetLastError());
    if (hBitmap == NULL)
    {
        DeleteDC(hdc);
        IDWriteFactory_Release(Factory);
        return;
    }
    hOld = SelectObject(hdc, hBitmap);

    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        for (Pixels = 12; Pixels <= 28; Pixels += 4)
        {
            if (!GdiWidths(hdc, Bits, &Cases[i], Pixels, &Extent, &Ink, Face))
            {
                ok(FALSE, "CreateFontW '%ls' failed\n", Cases[i].GdiFace);
                continue;
            }
            DWrite = DWriteWidth(Cases[i].Family, Cases[i].Weight, Cases[i].Text, Pixels);
            DWriteFace = DWriteWidth(Cases[i].GdiFace, DWRITE_FONT_WEIGHT_NORMAL, Cases[i].Text, Pixels);
            DWriteSelawik = DWriteWidth(L"Selawik", Cases[i].Weight, Cases[i].Text, Pixels);
            GdiPlus = GdiPlusWidth(hdc, Cases[i].GdiFace, Cases[i].Text, Pixels, GpName);

            trace("%lu %dpx GDI '%ls'->'%ls' extent %ld ink %ld | DWrite '%ls' w%u %.1f, '%ls' %.1f (Selawik %.1f) | GDI+ '%ls' %.1f\n",
                  i, Pixels, Cases[i].GdiFace, Face, Extent, Ink, Cases[i].Family, Cases[i].Weight,
                  DWrite, Cases[i].GdiFace, DWriteFace, DWriteSelawik, GpName, GdiPlus);

            ok(Ink <= Extent + 2, "%lu %dpx: GDI draws %ld px past its own extent %ld\n", i, Pixels, Ink - Extent, Extent);
            ok(DWrite + 2.0f + Ink / 25.0f >= Ink, "%lu %dpx: DirectWrite measures %.1f, GDI draws %ld\n", i, Pixels, DWrite, Ink);
            ok(DWriteFace + 2.0f + Ink / 50.0f >= Ink, "%lu %dpx: DirectWrite '%ls' measures %.1f, GDI draws %ld\n",
               i, Pixels, Cases[i].GdiFace, DWriteFace, Ink);
            if (GdiPlus >= 0.0f)
                ok(GdiPlus + 2.0f + Ink / 25.0f >= Ink, "%lu %dpx: GDI+ measures %.1f, GDI draws %ld\n", i, Pixels, GdiPlus, Ink);
        }
    }

    SelectObject(hdc, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdc);
    IDWriteFactory_Release(Factory);
}
