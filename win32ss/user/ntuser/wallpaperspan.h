/*
 * PROJECT:     LiberNT Win32k
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wallpaper Span mode geometry: one scale and crop for the virtual canvas.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#pragma once

typedef struct _WALLPAPER_SPAN_LAYOUT
{
    LONG ScaleNum;
    LONG ScaleDen;
    RECTL Source;
} WALLPAPER_SPAN_LAYOUT;

BOOL
WallpaperSpanLayout(
    _In_ LONG ImageCx,
    _In_ LONG ImageCy,
    _In_ const RECTL *VirtualBounds,
    _Out_ WALLPAPER_SPAN_LAYOUT *Layout);

BOOL
WallpaperSpanSample(
    _In_ const WALLPAPER_SPAN_LAYOUT *Layout,
    _In_ LONG ImageCx,
    _In_ LONG ImageCy,
    _In_ const RECTL *VirtualBounds,
    _In_ const RECTL *Monitor,
    _Out_ RECTL *Dest,
    _Out_ RECTL *Source);
