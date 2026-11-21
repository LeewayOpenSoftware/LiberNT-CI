/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Renders a COLR v0 emoji through TranslateColorGlyphRun and the GDI bitmap render target
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#define COBJMACROS
#include <apitest.h>
#include <initguid.h>
#include <dwrite_2.h>

#define SIZE 128

static UINT CountColours(HDC hdc, UINT *pcPainted, UINT *pcSaturated, UINT *pcYellow)
{
    DIBSECTION ds;
    HGDIOBJ hbm = GetCurrentObject(hdc, OBJ_BITMAP);
    const UINT32 *bits;
    UINT32 seen[64];
    UINT cSeen = 0, i, k, painted = 0, saturated = 0, yellow = 0;

    if (!GetObjectW(hbm, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32)
        return 0;
    bits = ds.dsBm.bmBits;
    for (i = 0; i < SIZE * SIZE; i++)
    {
        UINT32 px = bits[i] & 0xFFFFFF;
        if (px == 0xFFFFFF)
            continue;
        painted++;
        {
            UINT r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            UINT hi = max(r, max(g, b)), lo = min(r, min(g, b));
            if (hi - lo > 80)
                saturated++;
            if (r > 200 && g > 150 && b < 120)
                yellow++;
        }
        for (k = 0; k < cSeen && seen[k] != px; k++)
            ;
        if (k == cSeen && cSeen < ARRAYSIZE(seen))
            seen[cSeen++] = px;
    }
    *pcPainted = painted;
    *pcSaturated = saturated;
    *pcYellow = yellow;
    return cSeen;
}

START_TEST(ColorGlyphRender)
{
    WCHAR path[MAX_PATH];
    IDWriteFactory2 *factory = NULL;
    IDWriteFontFile *file = NULL;
    IDWriteFontFace *face = NULL;
    IDWriteGdiInterop *interop = NULL;
    IDWriteBitmapRenderTarget *target = NULL;
    IDWriteRenderingParams *params = NULL;
    IDWriteColorGlyphRunEnumerator *layers = NULL;
    UINT32 codepoint = 0x1F600;
    UINT16 glyph = 0;
    FLOAT advance = 0;
    DWRITE_GLYPH_RUN run;
    HRESULT hr;
    HDC hdc;
    RECT rc = { 0, 0, SIZE, SIZE };
    BOOL hasRun = FALSE;
    UINT nLayers = 0, nColours, nPainted = 0, nSaturated = 0, nYellow = 0;

    GetWindowsDirectoryW(path, ARRAYSIZE(path));
    lstrcatW(path, L"\\Fonts\\TwemojiMozilla.ttf");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    {
        skip("TwemojiMozilla.ttf is not installed\n");
        return;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2, (IUnknown **)&factory);
    ok(hr == S_OK, "DWriteCreateFactory failed: 0x%lx\n", hr);
    if (hr != S_OK)
        return;

    hr = IDWriteFactory2_CreateFontFileReference(factory, path, NULL, &file);
    ok(hr == S_OK, "CreateFontFileReference failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;
    hr = IDWriteFactory2_CreateFontFace(factory, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0,
                                        DWRITE_FONT_SIMULATIONS_NONE, &face);
    ok(hr == S_OK, "CreateFontFace failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;

    hr = IDWriteFontFace_GetGlyphIndices(face, &codepoint, 1, &glyph);
    ok(hr == S_OK, "GetGlyphIndices failed: 0x%lx\n", hr);
    ok(glyph != 0, "U+1F600 has no glyph\n");
    if (!glyph)
        goto done;

    memset(&run, 0, sizeof(run));
    run.fontFace = face;
    run.fontEmSize = 80.0f;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &advance;
    {
        DWRITE_GLYPH_METRICS gm;
        DWRITE_FONT_METRICS fm;
        IDWriteFontFace_GetMetrics(face, &fm);
        IDWriteFontFace_GetDesignGlyphMetrics(face, &glyph, 1, &gm, FALSE);
        advance = gm.advanceWidth * run.fontEmSize / fm.designUnitsPerEm;
    }

    hr = IDWriteFactory2_TranslateColorGlyphRun(factory, 10.0f, 100.0f, &run, NULL, DWRITE_MEASURING_MODE_NATURAL,
                                                NULL, 0, &layers);
    ok(hr == S_OK, "TranslateColorGlyphRun failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;

    hr = IDWriteFactory2_GetGdiInterop(factory, &interop);
    ok(hr == S_OK, "GetGdiInterop failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;
    hr = IDWriteGdiInterop_CreateBitmapRenderTarget(interop, NULL, SIZE, SIZE, &target);
    ok(hr == S_OK, "CreateBitmapRenderTarget failed: 0x%lx\n", hr);
    /* Aliased, so a coloured pixel can only come from a layer colour and not from ClearType fringes */
    hr = IDWriteFactory_CreateCustomRenderingParams((IDWriteFactory *)factory, 1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                     DWRITE_RENDERING_MODE_ALIASED, &params);
    ok(hr == S_OK, "CreateCustomRenderingParams failed: 0x%lx\n", hr);
    if (!target || !params)
        goto done;

    hdc = IDWriteBitmapRenderTarget_GetMemoryDC(target);
    FillRect(hdc, &rc, GetStockObject(WHITE_BRUSH));

    for (;;)
    {
        const DWRITE_COLOR_GLYPH_RUN *layer = NULL;
        COLORREF colour;
        BOOL more = FALSE;

        hr = IDWriteColorGlyphRunEnumerator_MoveNext(layers, &more);
        ok(hr == S_OK, "MoveNext failed: 0x%lx\n", hr);
        if (hr != S_OK || !more)
            break;
        hasRun = TRUE;
        hr = IDWriteColorGlyphRunEnumerator_GetCurrentRun(layers, &layer);
        ok(hr == S_OK && layer, "GetCurrentRun failed: 0x%lx\n", hr);
        if (!layer)
            break;
        nLayers++;
        if (layer->paletteIndex == 0xFFFF)
            colour = RGB(0, 0, 0);
        else
            colour = RGB((int)(layer->runColor.r * 255.0f + 0.5f), (int)(layer->runColor.g * 255.0f + 0.5f),
                         (int)(layer->runColor.b * 255.0f + 0.5f));
        hr = IDWriteBitmapRenderTarget_DrawGlyphRun(target, layer->baselineOriginX, layer->baselineOriginY,
                                                    DWRITE_MEASURING_MODE_NATURAL, &layer->glyphRun, params,
                                                    colour, NULL);
        ok(hr == S_OK, "DrawGlyphRun failed: 0x%lx\n", hr);
    }

    ok(hasRun, "TranslateColorGlyphRun returned no layers for a COLR glyph\n");
    ok(nLayers > 1, "Expected more than one colour layer, got %u\n", nLayers);
    nColours = CountColours(hdc, &nPainted, &nSaturated, &nYellow);
    trace("layers=%u painted=%u distinct colours=%u saturated=%u yellow=%u\n", nLayers, nPainted, nColours, nSaturated, nYellow);
    ok(nPainted > 0, "Nothing was painted\n");
    ok(nSaturated > 100, "Expected coloured (non-grey) pixels, got %u\n", nSaturated);
    ok(nYellow > 500, "Expected the yellow face of U+1F600, got %u yellow pixels\n", nYellow);
    ok(nColours > 1, "Expected more than one distinct non-background colour, got %u\n", nColours);

done:
    if (params) IDWriteRenderingParams_Release(params);
    if (target) IDWriteBitmapRenderTarget_Release(target);
    if (interop) IDWriteGdiInterop_Release(interop);
    if (layers) IDWriteColorGlyphRunEnumerator_Release(layers);
    if (face) IDWriteFontFace_Release(face);
    if (file) IDWriteFontFile_Release(file);
    if (factory) IDWriteFactory2_Release(factory);
}
