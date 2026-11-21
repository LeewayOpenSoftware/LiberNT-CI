/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that DirectWrite resolves Segoe UI Emoji and emoji text to Twemoji Mozilla
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#define COBJMACROS
#include <apitest.h>
#include <initguid.h>
#include <dwrite_2.h>

typedef struct
{
    IDWriteTextAnalysisSource IDWriteTextAnalysisSource_iface;
    const WCHAR *text;
    UINT32 length;
} EMOJI_SOURCE;

static EMOJI_SOURCE *impl_from_source(IDWriteTextAnalysisSource *iface)
{
    return CONTAINING_RECORD(iface, EMOJI_SOURCE, IDWriteTextAnalysisSource_iface);
}

static HRESULT WINAPI Source_QueryInterface(IDWriteTextAnalysisSource *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IDWriteTextAnalysisSource) || IsEqualIID(riid, &IID_IUnknown))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Source_AddRef(IDWriteTextAnalysisSource *iface) { return 2; }
static ULONG WINAPI Source_Release(IDWriteTextAnalysisSource *iface) { return 1; }

static HRESULT WINAPI Source_GetTextAtPosition(IDWriteTextAnalysisSource *iface, UINT32 position,
                                               const WCHAR **text, UINT32 *length)
{
    EMOJI_SOURCE *This = impl_from_source(iface);
    *text = position < This->length ? This->text + position : NULL;
    *length = position < This->length ? This->length - position : 0;
    return S_OK;
}

static HRESULT WINAPI Source_GetTextBeforePosition(IDWriteTextAnalysisSource *iface, UINT32 position,
                                                   const WCHAR **text, UINT32 *length)
{
    EMOJI_SOURCE *This = impl_from_source(iface);
    *text = position && position <= This->length ? This->text : NULL;
    *length = position <= This->length ? position : 0;
    return S_OK;
}

static DWRITE_READING_DIRECTION WINAPI Source_GetParagraphReadingDirection(IDWriteTextAnalysisSource *iface)
{
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

static HRESULT WINAPI Source_GetLocaleName(IDWriteTextAnalysisSource *iface, UINT32 position,
                                           UINT32 *length, const WCHAR **locale)
{
    static const WCHAR en[] = L"en-us";
    *locale = en;
    *length = ARRAYSIZE(en) - 1;
    return S_OK;
}

static HRESULT WINAPI Source_GetNumberSubstitution(IDWriteTextAnalysisSource *iface, UINT32 position,
                                                   UINT32 *length, IDWriteNumberSubstitution **substitution)
{
    *substitution = NULL;
    *length = 0;
    return S_OK;
}

static IDWriteTextAnalysisSourceVtbl SourceVtbl =
{
    Source_QueryInterface, Source_AddRef, Source_Release, Source_GetTextAtPosition,
    Source_GetTextBeforePosition, Source_GetParagraphReadingDirection, Source_GetLocaleName,
    Source_GetNumberSubstitution,
};

static BOOL FamilyNameOf(IDWriteFont *font, WCHAR *name, UINT32 cch)
{
    IDWriteFontFamily *family = NULL;
    IDWriteLocalizedStrings *names = NULL;
    BOOL got = FALSE;

    if (SUCCEEDED(IDWriteFont_GetFontFamily(font, &family)) &&
        SUCCEEDED(IDWriteFontFamily_GetFamilyNames(family, &names)) &&
        SUCCEEDED(IDWriteLocalizedStrings_GetString(names, 0, name, cch)))
        got = TRUE;
    if (names) IDWriteLocalizedStrings_Release(names);
    if (family) IDWriteFontFamily_Release(family);
    return got;
}

START_TEST(EmojiFallback)
{
    IDWriteFactory2 *factory = NULL;
    IDWriteFontCollection *collection = NULL;
    IDWriteFontFallback *fallback = NULL;
    UINT32 index = 0, mapped = 0;
    BOOL exists = FALSE;
    HRESULT hr;
    WCHAR name[64];
    FLOAT scale = 0;
    static const WCHAR text[] = { 0xD83D, 0xDE00, 0 }; /* U+1F600 */
    EMOJI_SOURCE source = { { &SourceVtbl }, text, 2 };
    IDWriteFont *font = NULL;
    IDWriteFontFamily *family = NULL;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2, (IUnknown **)&factory);
    ok(hr == S_OK, "DWriteCreateFactory failed: 0x%lx\n", hr);
    if (hr != S_OK)
        return;

    hr = IDWriteFactory2_GetSystemFontCollection(factory, &collection, FALSE);
    ok(hr == S_OK, "GetSystemFontCollection failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;

    hr = IDWriteFontCollection_FindFamilyName(collection, L"Twemoji Mozilla", &index, &exists);
    ok(hr == S_OK, "FindFamilyName failed: 0x%lx\n", hr);
    if (!exists)
    {
        skip("Twemoji Mozilla is not installed\n");
        goto done;
    }

    /* DirectWrite wants the colour font, whatever GDI's FontSubstitutes row says */
    exists = FALSE;
    hr = IDWriteFontCollection_FindFamilyName(collection, L"Segoe UI Emoji", &index, &exists);
    ok(hr == S_OK && exists, "Segoe UI Emoji not found: 0x%lx exists=%d\n", hr, exists);
    if (exists)
    {
        hr = IDWriteFontCollection_GetFontFamily(collection, index, &family);
        ok(hr == S_OK, "GetFontFamily failed: 0x%lx\n", hr);
        if (family)
        {
            IDWriteLocalizedStrings *names = NULL;
            IDWriteFontFamily_GetFamilyNames(family, &names);
            name[0] = 0;
            if (names)
            {
                IDWriteLocalizedStrings_GetString(names, 0, name, ARRAYSIZE(name));
                IDWriteLocalizedStrings_Release(names);
            }
            ok(!wcscmp(name, L"Twemoji Mozilla"), "Segoe UI Emoji resolved to %ls\n", name);
            IDWriteFontFamily_Release(family);
        }
    }

    hr = IDWriteFactory2_GetSystemFontFallback(factory, &fallback);
    ok(hr == S_OK, "GetSystemFontFallback failed: 0x%lx\n", hr);
    if (hr != S_OK)
        goto done;
    hr = IDWriteFontFallback_MapCharacters(fallback, &source.IDWriteTextAnalysisSource_iface, 0, 2, NULL, NULL,
                                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale);
    ok(hr == S_OK, "MapCharacters failed: 0x%lx\n", hr);
    ok(mapped == 2, "MapCharacters mapped %u of 2 units\n", mapped);
    ok(font != NULL, "U+1F600 was not mapped to a font\n");
    if (font)
    {
        name[0] = 0;
        ok(FamilyNameOf(font, name, ARRAYSIZE(name)), "No family name\n");
        ok(!wcscmp(name, L"Twemoji Mozilla"), "U+1F600 mapped to %ls\n", name);
        IDWriteFont_Release(font);
    }

done:
    if (fallback) IDWriteFontFallback_Release(fallback);
    if (collection) IDWriteFontCollection_Release(collection);
    IDWriteFactory2_Release(factory);
}
