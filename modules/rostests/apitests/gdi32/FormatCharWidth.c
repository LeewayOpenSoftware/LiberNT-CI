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
    0x202A, 0x202B, 0x202C, 0x202D, 0x202E,
    0x2060, 0x2066, 0x2067, 0x2068, 0x2069, 0xFEFF
};

static
LONG
InkRight(
    _In_ HDC hdc,
    _In_ PULONG Bits,
    _In_reads_(Count) PCWSTR Text,
    _In_ UINT Count)
{
    RECT Rect = { 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT };
    LONG x, y, Right = -1;

    FillRect(hdc, &Rect, GetStockObject(WHITE_BRUSH));
    ExtTextOutW(hdc, 4, 4, 0, NULL, Text, Count, NULL);
    GdiFlush();

    for (y = 0; y < CANVAS_HEIGHT; y++)
    {
        for (x = CANVAS_WIDTH - 1; x > Right; x--)
        {
            if ((Bits[y * CANVAS_WIDTH + x] & 0xFFFFFF) != 0xFFFFFF)
            {
                Right = x;
                break;
            }
        }
    }

    return Right;
}

START_TEST(FormatCharWidth)
{
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), CANVAS_WIDTH, -CANVAS_HEIGHT, 1, 32, BI_RGB } };
    WCHAR Text[ARRAYSIZE(Base) + 2];
    HBITMAP hBitmap, hOldBitmap;
    HFONT hFont, hOldFont;
    PULONG Bits = NULL;
    SIZE BaseSize, Size;
    LONG BaseInk, Ink;
    INT Fit;
    UINT Count, i;
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

    ok(GetTextExtentPoint32W(hdc, Base, ARRAYSIZE(Base) - 1, &BaseSize), "GetTextExtentPoint32W failed\n");
    BaseInk = InkRight(hdc, Bits, Base, ARRAYSIZE(Base) - 1);

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

        Ink = InkRight(hdc, Bits, Text, Count);
        ok(Ink <= BaseInk + 1, "U+%04X: ExtTextOutW draws to %ld, without the marks to %ld\n",
           Marks[i], Ink, BaseInk);
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdc, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdc);
}
