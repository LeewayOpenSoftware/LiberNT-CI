/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Stock Aureline icon resolver (G10): loads pre-rasterised .ico assets
 *              for ribbon_icons.h ids. No SVG at runtime.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <windows.h>
#include <leewayui/aureline_icons.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* icon_id -> "app/aur.app.slug" (generated from the masters). */
#include "aureline_slugs.inc"

static const char *RelForId(UINT32 id)
{
    for (size_t i = 0; i < sizeof(kAurelineSlugs) / sizeof(kAurelineSlugs[0]); i++)
        if (kAurelineSlugs[i].id == id)
            return kAurelineSlugs[i].rel;
    return NULL;
}

HICON
LeewayRibbonAurelineResolver(UINT32 icon_id, UINT32 size_px, LeewayUIIconVariant variant,
                             COLORREF fg, COLORREF bg, void *resolver_context)
{
    (void)fg; (void)bg;
    LeewayAurelineConfig *cfg = (LeewayAurelineConfig *)resolver_context;
    if (!cfg || !cfg->root)
        return NULL;

    const char *rel = RelForId(icon_id & LWUI_ICON_ID_MASK);
    if (!rel)
        return NULL;

    WCHAR relw[160];
    MultiByteToWideChar(CP_UTF8, 0, rel, -1, relw, 160);

    /* Try the requested variant first, then fall back as configured. */
    const WCHAR *suffix[2]; int ns = 0;
    if (variant == LWUI_ICON_VARIANT_MONO) {
        suffix[ns++] = L".mono.ico";
        if (cfg->mono_fallback_to_colour) suffix[ns++] = L".ico";
    } else {
        suffix[ns++] = L".ico";
    }

    for (int i = 0; i < ns; i++) {
        WCHAR path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%s\\%s%s", cfg->root, relw, suffix[i]);
        path[MAX_PATH - 1] = 0;
        HICON h = (HICON)LoadImageW(NULL, path, IMAGE_ICON,
                                    (int)size_px, (int)size_px,
                                    LR_LOADFROMFILE | LR_DEFAULTCOLOR);
        if (h)
            return h;
    }
    return NULL;
}

HRESULT WINAPI
LeewayRibbonUseAurelineIcons(ILeewayRibbon *ribbon, LeewayAurelineConfig *cfg)
{
    if (!ribbon || !cfg)
        return E_INVALIDARG;
    return LeewayRibbonSetIconResolver(ribbon, LeewayRibbonAurelineResolver, cfg);
}
