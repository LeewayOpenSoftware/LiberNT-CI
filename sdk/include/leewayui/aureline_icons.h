/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Stock Aureline icon resolver (G10). A ready-made LeewayUIIconResolver
 *              that loads pre-rasterised Aureline icon files for the icon_ids in
 *              ribbon_icons.h, so an app gets real ribbon icons with one call and
 *              no runtime SVG dependency. The committed assets are plain .ico files
 *              produced from the SVG masters by a build-time rasteriser (see
 *              tools/); this loader never parses SVG.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Asset layout under <root>:  <app>/aur.<app>.<slug>.ico        (colour, 16+32)
 *                             <app>/aur.<app>.<slug>.mono.ico   (mono, optional)
 * Pass the resolver to LeewayRibbonSetIconResolver with resolver_context = a
 * LeewayAurelineConfig* that stays alive for the ribbon's lifetime.
 */
#pragma once
#include <windows.h>
#include <leewayui/ribbon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const WCHAR *root;      /* directory holding the per-app icon folders */
    BOOL         mono_fallback_to_colour; /* if a .mono.ico is missing, use colour */
} LeewayAurelineConfig;

/* A LeewayUIIconResolver: resolver_context = LeewayAurelineConfig*. Returns an
 * HICON the ribbon owns (DestroyIcon on eviction), or NULL (placeholder glyph)
 * for an unknown id or a missing file. */
HICON LeewayRibbonAurelineResolver(UINT32 icon_id, UINT32 size_px,
                                          LeewayUIIconVariant variant,
                                          COLORREF fg, COLORREF bg,
                                          void *resolver_context);

/* Convenience: install the Aureline resolver on a ribbon. cfg must outlive it. */
HRESULT WINAPI LeewayRibbonUseAurelineIcons(ILeewayRibbon *ribbon, LeewayAurelineConfig *cfg);

#ifdef __cplusplus
}
#endif
