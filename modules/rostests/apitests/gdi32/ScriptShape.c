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

/* Windows-parity coverage check (Win11 probe, Consolas vs Nirmala UI):
 * ScriptShape returns USP_E_SCRIPT_NOT_IN_FONT only when the font covers NO
 * character of the run; partial coverage returns S_OK with .notdef for the
 * missing ones. Default-ignorable format characters never count. */

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
    /* ka + HEBREW ALEPH (not in a Devanagari font) + ssa; all-covered probes use items[0].iCharPos..items[1].iCharPos lengths */
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

    /* ZWJ discrimination (Dwight NIT): if the font HAS a ZWJ glyph this case
     * cannot prove the skip, so probe it and trace instead of asserting. */
    {
        WORD zwjGlyph = 0;
        if (ScriptGetCMap(hdc, &cache, runDevanagariZwj + 1, 1, 0, &zwjGlyph) != S_FALSE)
            trace("Noto Sans Devanagari has a ZWJ glyph; the ZWJ case does not discriminate\n");
    }

    /* Run with ZWJ: shapes even though the font has no ZWJ glyph */
    hr = ScriptShape(hdc, &cache, runDevanagariZwj, 3, 16, &items[0].a,
                     glyphs, logClust, sva, &cGlyphs);
    ok(hr == S_OK, "ScriptShape(ZWJ run) = 0x%lx, expected S_OK\n", hr);

    /* Partial coverage: S_OK with .notdef (Windows behaviour) */
    hr = ScriptItemize(runDevanagariMissing, 3, 4, &control, &state, items, &nItems);
    ok(hr == S_OK, "ScriptItemize(3) failed 0x%lx\n", hr);
    if (hr == S_OK)
    {
        hr = ScriptShape(hdc, &cache, runDevanagariMissing, 3, 16, &items[0].a,
                         glyphs, logClust, sva, &cGlyphs);
        ok(hr == S_OK, "ScriptShape(partial run) = 0x%lx, expected S_OK\n", hr);
    }

    /* Zero coverage: a Hebrew-only run under a Devanagari-only font must fail */
    {
        static const WCHAR heRun[] = { 0x05D0, 0x05D1, 0 };
        hr = ScriptItemize(heRun, 2, 4, &control, &state, items, &nItems);
        ok(hr == S_OK, "ScriptItemize(he) failed 0x%lx\n", hr);
        if (hr == S_OK)
        {
            hr = ScriptShape(hdc, &cache, heRun, 2, 16, &items[0].a,
                             glyphs, logClust, sva, &cGlyphs);
            ok(hr == USP_E_SCRIPT_NOT_IN_FONT,
               "ScriptShape(zero coverage) = 0x%lx, expected USP_E_SCRIPT_NOT_IN_FONT\n", hr);
        }
    }

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
