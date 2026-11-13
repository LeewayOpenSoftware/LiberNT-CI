/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that invisible format characters take no room in GDI text
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define CANVAS_WIDTH 640
#define CANVAS_HEIGHT 48

static const WCHAR Base[] = L"Se produjo un error";

static const WCHAR Marks[] =
{
    0x200B, 0x200C, 0x200D, 0x200E, 0x200F,
    0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0xFEFF
};

static
ULONG
DrawInk(
    _In_ HDC hdc,
    _In_ PULONG Bits,
    _In_reads_(Count) PCWSTR Text,
    _In_ UINT Count,
    _In_opt_ const INT *Dx,
    _Out_ PLONG Right)
{
    RECT Rect = { 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT };
    ULONG Pixels = 0;
    LONG x, y;

    FillRect(hdc, &Rect, GetStockObject(WHITE_BRUSH));
    ExtTextOutW(hdc, 4, 4, 0, NULL, Text, Count, Dx);
    GdiFlush();

    *Right = -1;
    for (y = 0; y < CANVAS_HEIGHT; y++)
    {
        for (x = 0; x < CANVAS_WIDTH; x++)
        {
            if ((Bits[y * CANVAS_WIDTH + x] & 0xFFFFFF) == 0xFFFFFF)
                continue;

            Pixels++;
            if (x > *Right)
                *Right = x;
        }
    }

    return Pixels;
}

static
VOID
TestMarksAroundText(
    _In_ HDC hdc,
    _In_ PULONG Bits)
{
    WCHAR Text[ARRAYSIZE(Base) + 2];
    SIZE BaseSize, Size;
    LONG BaseInk, Ink;
    INT Fit;
    UINT Count, i;

    ok(GetTextExtentPoint32W(hdc, Base, ARRAYSIZE(Base) - 1, &BaseSize), "GetTextExtentPoint32W failed\n");
    DrawInk(hdc, Bits, Base, ARRAYSIZE(Base) - 1, NULL, &BaseInk);

    for (i = 0; i < ARRAYSIZE(Marks); i++)
    {
        Text[0] = Marks[i];
        memcpy(&Text[1], Base, (ARRAYSIZE(Base) - 1) * sizeof(WCHAR));
        Text[ARRAYSIZE(Base)] = Marks[i];
        Count = ARRAYSIZE(Base) + 1;

        ok(GetTextExtentPoint32W(hdc, Text, Count, &Size), "GetTextExtentPoint32W failed\n");
        ok(Size.cx == BaseSize.cx, "U+%04X: GetTextExtentPoint32W gives %ld, without the marks %ld\n",
           Marks[i], Size.cx, BaseSize.cx);

        Fit = -1;
        ok(GetTextExtentExPointW(hdc, Text, Count, 10000, &Fit, NULL, &Size), "GetTextExtentExPointW failed\n");
        ok(Size.cx == BaseSize.cx, "U+%04X: GetTextExtentExPointW gives %ld, without the marks %ld\n",
           Marks[i], Size.cx, BaseSize.cx);
        ok(Fit == (INT)Count, "U+%04X: GetTextExtentExPointW fits %d of %u\n", Marks[i], Fit, Count);

        DrawInk(hdc, Bits, Text, Count, NULL, &Ink);
        ok(Ink <= BaseInk + 1, "U+%04X: ExtTextOutW draws to %ld, without the marks to %ld\n",
           Marks[i], Ink, BaseInk);
    }
}

static
VOID
TestMarksOnlyUpdateCp(
    _In_ HDC hdc)
{
    static const WCHAR Lrm[] = { 0x200E };
    POINT Point = { -1, -1 };

    SetTextAlign(hdc, TA_UPDATECP);
    MoveToEx(hdc, 10, 4, NULL);
    ok(TextOutW(hdc, 0, 0, Lrm, ARRAYSIZE(Lrm)), "TextOutW failed\n");
    ok(GetCurrentPositionEx(hdc, &Point), "GetCurrentPositionEx failed\n");
    ok(Point.x == 10 && Point.y == 4, "a lone U+200E moved the current position to %ld,%ld\n", Point.x, Point.y);
    SetTextAlign(hdc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
}

static
VOID
TestMarkWithDx(
    _In_ HDC hdc,
    _In_ PULONG Bits)
{
    static const WCHAR Plain[] = L"AB";
    static const WCHAR LeadMarks[] = { 0x200E, 0x200F };
    WCHAR Text[3];
    INT PlainDx[2], MarkedDx[3], Extents[2];
    SIZE Size;
    LONG PlainInk, Ink;
    UINT i;

    ok(GetTextExtentExPointW(hdc, Plain, 2, 0, NULL, Extents, &Size), "GetTextExtentExPointW failed\n");
    PlainDx[0] = Extents[0];
    PlainDx[1] = Extents[1] - Extents[0];
    DrawInk(hdc, Bits, Plain, 2, PlainDx, &PlainInk);

    for (i = 0; i < ARRAYSIZE(LeadMarks); i++)
    {
        Text[0] = LeadMarks[i];
        Text[1] = Plain[0];
        Text[2] = Plain[1];
        MarkedDx[0] = 0;
        MarkedDx[1] = PlainDx[0];
        MarkedDx[2] = PlainDx[1];

        DrawInk(hdc, Bits, Text, 3, MarkedDx, &Ink);
        ok(Ink >= PlainInk - 1 && Ink <= PlainInk + 1, "U+%04X with Dx: ExtTextOutW draws to %ld, without it to %ld\n",
           LeadMarks[i], Ink, PlainInk);
    }
}

static
HFONT
CreateHebrewFont(
    _In_ HDC hdc)
{
    static const PCWSTR Faces[] = { L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans", L"FreeSans" };
    static const WCHAR Needed[] = { 0x05E9, 0x05B8, 0x05DC, 0x05D5, 0x05DD };
    WORD Glyphs[ARRAYSIZE(Needed)];
    HFONT hFont, hOldFont;
    UINT i, j;

    for (i = 0; i < ARRAYSIZE(Faces); i++)
    {
        hFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, Faces[i]);
        if (hFont == NULL)
            continue;

        hOldFont = SelectObject(hdc, hFont);
        j = 0;
        if (GetGlyphIndicesW(hdc, Needed, ARRAYSIZE(Needed), Glyphs, GGI_MARK_NONEXISTING_GLYPHS) == ARRAYSIZE(Needed))
        {
            while (j < ARRAYSIZE(Needed) && Glyphs[j] != 0xFFFF)
                j++;
        }
        SelectObject(hdc, hOldFont);

        if (j == ARRAYSIZE(Needed))
            return hFont;

        DeleteObject(hFont);
    }

    return NULL;
}

static
VOID
TestRtlCombiningMark(
    _In_ HDC hdc,
    _In_ PULONG Bits)
{
    static const WCHAR Bare[] = { 0x05E9, 0x05DC, 0x05D5, 0x05DD };
    static const WCHAR Pointed[] = { 0x05E9, 0x05B8, 0x05DC, 0x05D5, 0x05DD };
    ULONG BareInk, PointedInk;
    HFONT hFont, hOldFont;
    LONG Right;

    hFont = CreateHebrewFont(hdc);
    if (hFont == NULL)
    {
        skip("no installed font has the Hebrew letters and the qamats point\n");
        return;
    }
    hOldFont = SelectObject(hdc, hFont);

    BareInk = DrawInk(hdc, Bits, Bare, ARRAYSIZE(Bare), NULL, &Right);
    PointedInk = DrawInk(hdc, Bits, Pointed, ARRAYSIZE(Pointed), NULL, &Right);
    ok(BareInk > 0, "the Hebrew letters were not drawn\n");
    ok(PointedInk > BareInk, "the Hebrew point was not drawn: %lu ink pixels with it, %lu without\n",
       PointedInk, BareInk);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

START_TEST(FormatCharWidth)
{
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), CANVAS_WIDTH, -CANVAS_HEIGHT, 1, 32, BI_RGB } };
    HBITMAP hBitmap, hOldBitmap;
    HFONT hFont, hOldFont;
    PULONG Bits = NULL;
    HDC hdc;

    hdc = CreateCompatibleDC(NULL);
    hBitmap = CreateDIBSection(hdc, &Info, DIB_RGB_COLORS, (PVOID *)&Bits, NULL, 0);
    ok(hBitmap != NULL, "CreateDIBSection failed %lu\n", GetLastError());
    if (hBitmap == NULL)
    {
        DeleteDC(hdc);
        return;
    }
    hOldBitmap = SelectObject(hdc, hBitmap);

    hFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    ok(hFont != NULL, "CreateFontW failed\n");
    hOldFont = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    TestMarksAroundText(hdc, Bits);
    TestMarksOnlyUpdateCp(hdc);
    TestMarkWithDx(hdc, Bits);
    TestRtlCombiningMark(hdc, Bits);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdc, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdc);
}
