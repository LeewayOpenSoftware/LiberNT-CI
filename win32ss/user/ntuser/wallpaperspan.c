/*
 * PROJECT:     LiberNT Win32k
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wallpaper Span mode geometry: one scale and crop for the virtual canvas.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <windef.h>
#include "wallpaperspan.h"

static LONG
SpanMulDiv(
    _In_ LONG Number,
    _In_ LONG Numerator,
    _In_ LONG Denominator)
{
    LONGLONG Product = (LONGLONG)Number * Numerator;
    return (LONG)((Product + Denominator / 2) / Denominator);
}

BOOL
WallpaperSpanLayout(
    _In_ LONG ImageCx,
    _In_ LONG ImageCy,
    _In_ const RECTL *VirtualBounds,
    _Out_ WALLPAPER_SPAN_LAYOUT *Layout)
{
    LONG CanvasCx, CanvasCy, ScaledCx, ScaledCy;

    if (!VirtualBounds || !Layout)
        return FALSE;

    CanvasCx = VirtualBounds->right - VirtualBounds->left;
    CanvasCy = VirtualBounds->bottom - VirtualBounds->top;
    if (ImageCx <= 0 || ImageCy <= 0 || CanvasCx <= 0 || CanvasCy <= 0)
        return FALSE;

    /* The crop-to-fill computation of wmFill, against the virtual canvas. */
    if ((LONGLONG)CanvasCx * ImageCy > (LONGLONG)CanvasCy * ImageCx)
    {
        Layout->ScaleNum = CanvasCx;
        Layout->ScaleDen = ImageCx;
    }
    else
    {
        Layout->ScaleNum = CanvasCy;
        Layout->ScaleDen = ImageCy;
    }

    ScaledCx = SpanMulDiv(ImageCx, Layout->ScaleNum, Layout->ScaleDen);
    ScaledCy = SpanMulDiv(ImageCy, Layout->ScaleNum, Layout->ScaleDen);
    if (ScaledCx <= 0 || ScaledCy <= 0)
        return FALSE;

    Layout->Source.left = (LONG)(((LONGLONG)(ScaledCx - CanvasCx) * ImageCx) / (2 * ScaledCx));
    Layout->Source.top = (LONG)(((LONGLONG)(ScaledCy - CanvasCy) * ImageCy) / (2 * ScaledCy));
    Layout->Source.right = Layout->Source.left + (LONG)(((LONGLONG)CanvasCx * ImageCx) / ScaledCx);
    Layout->Source.bottom = Layout->Source.top + (LONG)(((LONGLONG)CanvasCy * ImageCy) / ScaledCy);
    return TRUE;
}

BOOL
WallpaperSpanSample(
    _In_ const WALLPAPER_SPAN_LAYOUT *Layout,
    _In_ LONG ImageCx,
    _In_ LONG ImageCy,
    _In_ const RECTL *VirtualBounds,
    _In_ const RECTL *Monitor,
    _Out_ RECTL *Dest,
    _Out_ RECTL *Source)
{
    LONG CanvasCx, CanvasCy, SourceCx, SourceCy;
    RECTL Clip;

    if (!Layout || !VirtualBounds || !Monitor || !Dest || !Source)
        return FALSE;

    CanvasCx = VirtualBounds->right - VirtualBounds->left;
    CanvasCy = VirtualBounds->bottom - VirtualBounds->top;
    if (ImageCx <= 0 || ImageCy <= 0 || CanvasCx <= 0 || CanvasCy <= 0)
        return FALSE;

    Clip = *Monitor;
    if (Clip.left < VirtualBounds->left)
        Clip.left = VirtualBounds->left;
    if (Clip.top < VirtualBounds->top)
        Clip.top = VirtualBounds->top;
    if (Clip.right > VirtualBounds->right)
        Clip.right = VirtualBounds->right;
    if (Clip.bottom > VirtualBounds->bottom)
        Clip.bottom = VirtualBounds->bottom;
    if (Clip.left >= Clip.right || Clip.top >= Clip.bottom)
        return FALSE;

    SourceCx = Layout->Source.right - Layout->Source.left;
    SourceCy = Layout->Source.bottom - Layout->Source.top;

    *Dest = Clip;
    Source->left = Layout->Source.left + (LONG)(((LONGLONG)(Clip.left - VirtualBounds->left) * SourceCx) / CanvasCx);
    Source->right = Layout->Source.left + (LONG)(((LONGLONG)(Clip.right - VirtualBounds->left) * SourceCx) / CanvasCx);
    Source->top = Layout->Source.top + (LONG)(((LONGLONG)(Clip.top - VirtualBounds->top) * SourceCy) / CanvasCy);
    Source->bottom = Layout->Source.top + (LONG)(((LONGLONG)(Clip.bottom - VirtualBounds->top) * SourceCy) / CanvasCy);
    return TRUE;
}
