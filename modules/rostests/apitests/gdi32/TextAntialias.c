/*
 * PROJECT:     LiberNT API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests that the dialog fonts are smoothed instead of drawn from bitmap strikes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define WIDTH  320
#define HEIGHT 40
#define TAG_EBLC 0x434C4245 /* 'EBLC' as GetFontData expects it */

static const PCWSTR g_Text = L"Turn on the firewall (not recommended)";

static HFONT CreatePointFont(HDC hdc, PCWSTR pszFace, INT nPoints, BYTE Quality)
{
    LOGFONTW lf;

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(nPoints, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = Quality;
    StringCchCopyW(lf.lfFaceName, _countof(lf.lfFaceName), pszFace);
    return CreateFontIndirectW(&lf);
}

/* Draw black text on white and count the gray levels and the colored edge pixels. */
static void MeasureText(HDC hdc, DWORD *pBits, HFONT hFont, UINT *pcLevels, UINT *pcColored)
{
    BYTE Seen[256];
    UINT i;

    ZeroMemory(Seen, sizeof(Seen));
    for (i = 0; i < WIDTH * HEIGHT; i++)
        pBits[i] = 0x00ffffff;

    SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    ExtTextOutW(hdc, 4, 4, 0, NULL, g_Text, lstrlenW(g_Text), NULL);
    GdiFlush();

    *pcColored = 0;
    for (i = 0; i < WIDTH * HEIGHT; i++)
    {
        BYTE r = GetRValue(pBits[i]), g = GetGValue(pBits[i]), b = GetBValue(pBits[i]);

        Seen[r] = Seen[g] = Seen[b] = 1;
        if (r != b)
            (*pcColored)++;
    }

    *pcLevels = 0;
    for (i = 1; i < 255; i++)
        *pcLevels += Seen[i];
}

static void TestFace(HDC hdc, DWORD *pBits, PCWSTR pszFace, INT nPoints)
{
    HFONT hFont, hOld;
    UINT cLevels, cColored;
    WCHAR szFace[LF_FACESIZE];

    /* A UI face must not carry embedded bitmap strikes: Windows' Tahoma and
       Microsoft Sans Serif have none, and a strike bypasses all smoothing. */
    hFont = CreatePointFont(hdc, pszFace, nPoints, DEFAULT_QUALITY);
    ok(hFont != NULL, "%ls: CreateFontIndirectW failed\n", pszFace);
    if (!hFont)
        return;
    hOld = SelectObject(hdc, hFont);
    GetTextFaceW(hdc, _countof(szFace), szFace);
    ok(GetFontData(hdc, TAG_EBLC, 0, NULL, 0) == GDI_ERROR,
       "%ls (%ls) %d pt: has an EBLC table\n", pszFace, szFace, nPoints);
    SelectObject(hdc, hOld);
    DeleteObject(hFont);

    /* ClearType ignores the gasp "no gray" ranges, so the outline must be
       smoothed at every UI size. Windows 11 gives 5 levels per channel and
       hundreds of colored edge pixels here; a 1-bit strike gives none. */
    hFont = CreatePointFont(hdc, pszFace, nPoints, CLEARTYPE_QUALITY);
    MeasureText(hdc, pBits, hFont, &cLevels, &cColored);
    ok(cLevels >= 3, "%ls %d pt ClearType: %u gray levels, expected at least 3\n",
       pszFace, nPoints, cLevels);
    ok(cColored > 0, "%ls %d pt ClearType: no subpixel color fringes\n", pszFace, nPoints);
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
    DeleteObject(hFont);
}

START_TEST(TextAntialias)
{
    static const PCWSTR Faces[] = { L"MS Shell Dlg 2", L"MS Shell Dlg", L"Tahoma", L"Microsoft Sans Serif" };
    static const INT Points[] = { 8, 9, 10, 11 };
    BITMAPINFO bmi;
    DWORD *pBits = NULL;
    HBITMAP hbm, hbmOld;
    HDC hdc;
    UINT i, j;

    hdc = CreateCompatibleDC(NULL);
    ok(hdc != NULL, "CreateCompatibleDC failed\n");
    if (!hdc)
        return;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = WIDTH;
    bmi.bmiHeader.biHeight = -HEIGHT;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (PVOID *)&pBits, NULL, 0);
    ok(hbm != NULL && pBits != NULL, "CreateDIBSection failed\n");
    if (!hbm || !pBits)
    {
        DeleteDC(hdc);
        return;
    }
    hbmOld = SelectObject(hdc, hbm);

    for (i = 0; i < _countof(Faces); i++)
    {
        for (j = 0; j < _countof(Points); j++)
            TestFace(hdc, pBits, Faces[i], Points[j]);
    }

    SelectObject(hdc, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdc);
}
