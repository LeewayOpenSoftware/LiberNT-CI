/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     usp10 ScriptShape no-glyph coverage check (LiberNT HE/HI fix)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <apitest.h>
#include <windows.h>
#include <usp10.h>

/* The LiberNT coverage check in ScriptShapeOpenType must ignore
 * default-ignorable format characters (ZWJ/ZWNJ, LRM/RLM, the embedding
 * controls, invisible operators, ZWNBSP, soft hyphen): a Devanagari run
 * containing ZWJ must still shape (S_OK) with a Devanagari font even when
 * that font has no ZWJ glyph, while a run with a genuinely uncovered
 * character must fail with USP_E_SCRIPT_NOT_IN_FONT. */

static void test_scriptshape_coverage(void)
{
    HDC hdc;
    HFONT hFont, hOld;
    LOGFONTW lf;
    SCRIPT_CONTROL control;
    SCRIPT_STATE state;
    SCRIPT_ITEM items[4];
    int nItems;
    SCRIPT_CACHE cache = NULL;
    WORD glyphs[16], logClust[16];
    SCRIPT_VISATTR sva[16];
    int cGlyphs;
    HRESULT hr;
    /* ka + ZWJ + ssa */
    static const WCHAR runDevanagariZwj[] = { 0x0915, 0x200D, 0x0937, 0 };
    /* ka + HEBREW ALEPH (not in a Devanagari font) + ssa */
    static const WCHAR runDevanagariMissing[] = { 0x0915, 0x05D0, 0x0937, 0 };

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -16;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH;
    wcscpy(lf.lfFaceName, L"Noto Sans Devanagari");
    hFont = CreateFontIndirectW(&lf);
    if (!hFont)
    {
        skip("CreateFontIndirectW failed\n");
        return;
    }

    hdc = CreateCompatibleDC(NULL);
    hOld = (HFONT)SelectObject(hdc, hFont);

    /* Confirm the face really resolved to Noto Sans Devanagari */
    {
        WCHAR face[LF_FACESIZE];
        GetTextFaceW(hdc, LF_FACESIZE, face);
        if (wcscmp(face, L"Noto Sans Devanagari") != 0)
        {
            skip("Noto Sans Devanagari not installed (got %ls)\n", face);
            SelectObject(hdc, hOld);
            DeleteObject(hFont);
            DeleteDC(hdc);
            return;
        }
    }

    memset(&control, 0, sizeof(control));
    memset(&state, 0, sizeof(state));
    hr = ScriptItemize(runDevanagariZwj, 3, 4, &control, &state, items, &nItems);
    ok(hr == S_OK, "ScriptItemize failed 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;

    /* Run with ZWJ: shapes even though the font has no ZWJ glyph */
    hr = ScriptShape(hdc, &cache, runDevanagariZwj, 3, 16, &items[0].a,
                     glyphs, logClust, sva, &cGlyphs);
    ok(hr == S_OK, "ScriptShape(ZWJ run) = 0x%lx, expected S_OK\n", hr);

    /* Run with an uncovered character: must fail with NOT_IN_FONT */
    hr = ScriptItemize(runDevanagariMissing, 3, 4, &control, &state, items, &nItems);
    ok(hr == S_OK, "ScriptItemize(2) failed 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;
    hr = ScriptShape(hdc, &cache, runDevanagariMissing, 3, 16, &items[0].a,
                     glyphs, logClust, sva, &cGlyphs);
    ok(hr == USP_E_SCRIPT_NOT_IN_FONT,
       "ScriptShape(missing glyph run) = 0x%lx, expected USP_E_SCRIPT_NOT_IN_FONT\n", hr);

done:
    ScriptFreeCache(&cache);
    SelectObject(hdc, hOld);
    DeleteObject(hFont);
    DeleteDC(hdc);
}

START_TEST(ScriptShape)
{
    test_scriptshape_coverage();
}
