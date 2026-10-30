/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test for ExtTextOutW clipping
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

#define TEST_SIZE 96

static
ULONG
CountInk(
    _In_ const DWORD *Bits,
    _In_ const RECT *Inside,
    _In_ BOOL InsideRect)
{
    ULONG Count = 0;
    LONG X, Y;

    for (Y = 0; Y < TEST_SIZE; Y++)
    {
        for (X = 0; X < TEST_SIZE; X++)
        {
            BOOL In = (X >= Inside->left) && (X < Inside->right) && (Y >= Inside->top) && (Y < Inside->bottom);

            if ((In == InsideRect) && ((Bits[Y * TEST_SIZE + X] & 0x00FFFFFF) != 0x00FFFFFF))
                Count++;
        }
    }
    return Count;
}

static
VOID
DrawSample(
    _In_ HDC Dc,
    _Inout_ DWORD *Bits,
    _In_ UINT Options,
    _In_ const RECT *Clip)
{
    RECT Rect = *Clip;

    memset(Bits, 0xFF, TEST_SIZE * TEST_SIZE * sizeof(DWORD));
    GdiFlush();
    ok(ExtTextOutW(Dc, 2, 2, Options, &Rect, L"WMW", 3, NULL), "ExtTextOutW failed, error %lu\n", GetLastError());
    GdiFlush();
}

START_TEST(ExtTextOut)
{
    static const RECT Clip = { 20, 24, 70, 80 };
    BITMAPINFO Info = { { sizeof(BITMAPINFOHEADER), TEST_SIZE, -TEST_SIZE, 1, 32, BI_RGB } };
    RECT Above = { 0, 0, TEST_SIZE, Clip.top };
    RECT Left = { 0, 0, Clip.left, TEST_SIZE };
    DWORD *Bits = NULL;
    HBITMAP Bitmap, OldBitmap;
    HFONT Font, OldFont;
    HDC Dc;

    Dc = CreateCompatibleDC(NULL);
    ok(Dc != NULL, "CreateCompatibleDC failed\n");
    Bitmap = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, (PVOID *)&Bits, NULL, 0);
    ok(Bitmap != NULL && Bits != NULL, "CreateDIBSection failed\n");
    if (Dc == NULL || Bitmap == NULL || Bits == NULL)
        return;
    Font = CreateFontW(-48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Courier New");
    ok(Font != NULL, "CreateFontW failed\n");
    OldBitmap = SelectObject(Dc, Bitmap);
    OldFont = SelectObject(Dc, Font);
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, RGB(0, 0, 0));

    DrawSample(Dc, Bits, 0, &Clip);
    ok(CountInk(Bits, &Above, TRUE) != 0, "unclipped text does not reach above the clip rectangle\n");
    ok(CountInk(Bits, &Left, TRUE) != 0, "unclipped text does not reach left of the clip rectangle\n");

    DrawSample(Dc, Bits, ETO_CLIPPED, &Clip);
    ok_long(CountInk(Bits, &Clip, FALSE), 0);
    ok(CountInk(Bits, &Clip, TRUE) != 0, "clipped text drew nothing inside the clip rectangle\n");

    SelectObject(Dc, OldFont);
    SelectObject(Dc, OldBitmap);
    DeleteObject(Font);
    DeleteObject(Bitmap);
    DeleteDC(Dc);
}
