/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests per-glyph image formats of a COLR font and Direct2D colour text
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#define COBJMACROS
#include <apitest.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite_3.h>

#define SIZE 160

typedef HRESULT (WINAPI *D2D1CREATEFACTORY)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);

static BOOL GetTwemojiPath(WCHAR *path)
{
    GetWindowsDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\Fonts\\TwemojiMozilla.ttf");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void TestGlyphImageFormats(IDWriteFactory *factory, const WCHAR *path)
{
    IDWriteFontFile *file = NULL;
    IDWriteFontFace *face = NULL;
    IDWriteFontFace4 *face4 = NULL;
    DWRITE_GLYPH_IMAGE_FORMATS formats;
    DWRITE_GLYPH_IMAGE_DATA data;
    UINT32 codepoint = 0x1F600;
    UINT16 glyph = 0;
    void *context = (void *)1;
    HRESULT hr;

    hr = IDWriteFactory_CreateFontFileReference(factory, path, NULL, &file);
    ok(hr == S_OK, "CreateFontFileReference failed: 0x%lx\n", hr);
    if (hr != S_OK)
        return;
    hr = IDWriteFactory_CreateFontFace(factory, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0,
                                       DWRITE_FONT_SIMULATIONS_NONE, &face);
    ok(hr == S_OK, "CreateFontFace failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;
    hr = IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace4, (void **)&face4);
    if (hr != S_OK)
    {
        skip("IDWriteFontFace4 is not available\n");
        goto done;
    }

    IDWriteFontFace_GetGlyphIndices(face, &codepoint, 1, &glyph);
    ok(glyph != 0, "U+1F600 has no glyph\n");

    formats = IDWriteFontFace4_GetGlyphImageFormats(face4);
    ok(formats & DWRITE_GLYPH_IMAGE_FORMATS_COLR, "Face formats 0x%x lack COLR\n", formats);

    formats = 0;
    hr = IDWriteFontFace4_GetGlyphImageFormats_(face4, glyph, 0, 0xFFFF, &formats);
    ok(hr == S_OK, "GetGlyphImageFormats_ failed: 0x%lx\n", hr);
    ok(formats & DWRITE_GLYPH_IMAGE_FORMATS_COLR, "Glyph %u formats 0x%x lack COLR\n", glyph, formats);
    ok(formats & DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE, "Glyph %u formats 0x%x lack TRUETYPE\n", glyph, formats);

    formats = 0xFF;
    hr = IDWriteFontFace4_GetGlyphImageFormats_(face4, 0xFFFF, 0, 0xFFFF, &formats);
    ok(hr == S_OK, "GetGlyphImageFormats_ failed: 0x%lx\n", hr);
    ok(formats == DWRITE_GLYPH_IMAGE_FORMATS_NONE, "Invalid glyph formats 0x%x\n", formats);

    /* COLR layers and outlines are not image data */
    hr = IDWriteFontFace4_GetGlyphImageData(face4, glyph, 80, DWRITE_GLYPH_IMAGE_FORMATS_COLR, &data, &context);
    ok(hr == E_INVALIDARG, "GetGlyphImageData(COLR) returned 0x%lx\n", hr);
    ok(context == NULL && data.imageData == NULL, "COLR returned data\n");

    /* A format the font does not have gives an empty result */
    context = (void *)1;
    hr = IDWriteFontFace4_GetGlyphImageData(face4, glyph, 80, DWRITE_GLYPH_IMAGE_FORMATS_PNG, &data, &context);
    ok(hr == S_OK, "GetGlyphImageData(PNG) returned 0x%lx\n", hr);
    ok(data.imageData == NULL && data.imageDataSize == 0, "PNG returned data\n");
    IDWriteFontFace4_ReleaseGlyphImageData(face4, context);

    context = (void *)1;
    hr = IDWriteFontFace4_GetGlyphImageData(face4, glyph, 80, DWRITE_GLYPH_IMAGE_FORMATS_SVG, &data, &context);
    ok(hr == S_OK, "GetGlyphImageData(SVG) returned 0x%lx\n", hr);
    ok(data.imageData == NULL, "SVG returned data for a font without SVG\n");
    IDWriteFontFace4_ReleaseGlyphImageData(face4, context);

done:
    if (face4) IDWriteFontFace4_Release(face4);
    if (face) IDWriteFontFace_Release(face);
    if (file) IDWriteFontFile_Release(file);
}

static void CountPixels(HDC hdc, UINT *pYellow, UINT *pPainted)
{
    DIBSECTION ds;
    const UINT32 *bits;
    UINT i;

    *pYellow = *pPainted = 0;
    if (!GetObjectW(GetCurrentObject(hdc, OBJ_BITMAP), sizeof(ds), &ds) || !ds.dsBm.bmBits)
        return;
    bits = ds.dsBm.bmBits;
    for (i = 0; i < SIZE * SIZE; i++)
    {
        UINT32 px = bits[i] & 0xFFFFFF;
        UINT r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;

        if (px == 0xFFFFFF)
            continue;
        (*pPainted)++;
        if (r > 200 && g > 150 && b < 120)
            (*pYellow)++;
    }
}

static BOOL DrawEmoji(ID2D1Factory *d2d, IDWriteFactory *dwrite, D2D1_DRAW_TEXT_OPTIONS options, UINT *pYellow,
                      UINT *pPainted)
{
    static const WCHAR text[] = { 0xD83D, 0xDE00, 0 };
    D2D1_RENDER_TARGET_PROPERTIES props = { D2D1_RENDER_TARGET_TYPE_DEFAULT,
        { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT };
    ID2D1DCRenderTarget *dc = NULL;
    ID2D1SolidColorBrush *brush = NULL;
    IDWriteTextFormat *format = NULL;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), SIZE, -SIZE, 1, 32, BI_RGB } };
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    D2D1_RECT_F layout = { 0, 0, SIZE, SIZE };
    RECT rc = { 0, 0, SIZE, SIZE };
    HDC hdc;
    HBITMAP bmp;
    void *bits;
    HRESULT hr;
    BOOL ret = FALSE;

    hdc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(hdc, bmp);
    FillRect(hdc, &rc, GetStockObject(WHITE_BRUSH));

    hr = ID2D1Factory_CreateDCRenderTarget(d2d, &props, &dc);
    ok(hr == S_OK, "CreateDCRenderTarget failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto out;
    hr = ID2D1DCRenderTarget_BindDC(dc, hdc, &rc);
    ok(hr == S_OK, "BindDC failed: 0x%lx\n", hr);
    hr = ID2D1DCRenderTarget_CreateSolidColorBrush(dc, &black, NULL, &brush);
    ok(hr == S_OK, "CreateSolidColorBrush failed: 0x%lx\n", hr);
    hr = IDWriteFactory_CreateTextFormat(dwrite, L"Twemoji Mozilla", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 80.0f, L"en-us", &format);
    ok(hr == S_OK, "CreateTextFormat failed: 0x%lx\n", hr);
    if (!brush || !format)
        goto out;

    ID2D1DCRenderTarget_BeginDraw(dc);
    ID2D1DCRenderTarget_DrawText(dc, text, 2, format, &layout, (ID2D1Brush *)brush, options,
                                 DWRITE_MEASURING_MODE_NATURAL);
    hr = ID2D1DCRenderTarget_EndDraw(dc, NULL, NULL);
    ok(hr == S_OK, "EndDraw failed: 0x%lx\n", hr);
    GdiFlush();
    CountPixels(hdc, pYellow, pPainted);
    ret = TRUE;

out:
    if (format) IDWriteTextFormat_Release(format);
    if (brush) ID2D1SolidColorBrush_Release(brush);
    if (dc) ID2D1DCRenderTarget_Release(dc);
    DeleteObject(bmp);
    DeleteDC(hdc);
    return ret;
}

static void TestD2DColourText(IDWriteFactory *dwrite)
{
    HMODULE d2d1 = LoadLibraryW(L"d2d1.dll");
    D2D1CREATEFACTORY create;
    ID2D1Factory *d2d = NULL;
    UINT yellowOn = 0, paintedOn = 0, yellowOff = 0, paintedOff = 0;
    HRESULT hr;

    if (!d2d1 || !(create = (D2D1CREATEFACTORY)GetProcAddress(d2d1, "D2D1CreateFactory")))
    {
        skip("d2d1.dll is not available\n");
        goto done;
    }
    hr = create(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&d2d);
    ok(hr == S_OK, "D2D1CreateFactory failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;

    if (!DrawEmoji(d2d, dwrite, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT, &yellowOn, &paintedOn) ||
        !DrawEmoji(d2d, dwrite, D2D1_DRAW_TEXT_OPTIONS_NONE, &yellowOff, &paintedOff))
        goto done;
    trace("colour: painted=%u yellow=%u; plain: painted=%u yellow=%u\n", paintedOn, yellowOn, paintedOff, yellowOff);
    ok(paintedOn > 0, "Nothing was painted with ENABLE_COLOR_FONT\n");
    ok(yellowOn > 500, "ENABLE_COLOR_FONT drew %u yellow pixels\n", yellowOn);
    ok(yellowOff == 0, "Without ENABLE_COLOR_FONT %u yellow pixels were drawn\n", yellowOff);

done:
    if (d2d) ID2D1Factory_Release(d2d);
    if (d2d1) FreeLibrary(d2d1);
}

START_TEST(GlyphImageData)
{
    WCHAR path[MAX_PATH];
    IDWriteFactory *factory = NULL;
    HRESULT hr;

    if (!GetTwemojiPath(path))
    {
        skip("TwemojiMozilla.ttf is not installed\n");
        return;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&factory);
    ok(hr == S_OK, "DWriteCreateFactory failed: 0x%lx\n", hr);
    if (hr != S_OK)
        return;

    TestGlyphImageFormats(factory, path);
    TestD2DColourText(factory);
    IDWriteFactory_Release(factory);
}
