/*
 * PROJECT:     LiberNT Win32k
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wallpaper Span mode geometry: one scale and crop for the virtual canvas.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <limits.h>
#include <windef.h>
#include "wallpaperspan.h"

static BOOL
SpanLongFrom64(
    _In_ LONGLONG Value,
    _Out_ LONG *Result)
{
    if (Value < 0 || Value > LONG_MAX)
        return FALSE;

    *Result = (LONG)Value;
    return TRUE;
}

static BOOL
SpanMulDiv(
    _In_ LONG Number,
    _In_ LONG Numerator,
    _In_ LONG Denominator,
    _Out_ LONG *Result)
{
    LONGLONG Product;

    if (Number < 0 || Numerator < 0 || Denominator <= 0)
        return FALSE;

    Product = (LONGLONG)Number * Numerator;
    return SpanLongFrom64((Product + Denominator / 2) / Denominator, Result);
}

BOOL
WallpaperSpanLayout(
    _In_ LONG ImageCx,
    _In_ LONG ImageCy,
    _In_ const RECTL *VirtualBounds,
    _Out_ WALLPAPER_SPAN_LAYOUT *Layout)
{
    LONGLONG CanvasCx, CanvasCy;
    LONG ScaledCx, ScaledCy;
    RECTL Source;

    if (!VirtualBounds || !Layout)
        return FALSE;

    CanvasCx = (LONGLONG)VirtualBounds->right - VirtualBounds->left;
    CanvasCy = (LONGLONG)VirtualBounds->bottom - VirtualBounds->top;
    if (ImageCx <= 0 || ImageCy <= 0 ||
        CanvasCx <= 0 || CanvasCx > LONG_MAX ||
        CanvasCy <= 0 || CanvasCy > LONG_MAX)
        return FALSE;

    /* The crop-to-fill computation of wmFill, against the virtual canvas. */
    if (CanvasCx * ImageCy > CanvasCy * ImageCx)
    {
        Layout->ScaleNum = (LONG)CanvasCx;
        Layout->ScaleDen = ImageCx;
    }
    else
    {
        Layout->ScaleNum = (LONG)CanvasCy;
        Layout->ScaleDen = ImageCy;
    }

    if (!SpanMulDiv(ImageCx, Layout->ScaleNum, Layout->ScaleDen, &ScaledCx) ||
        !SpanMulDiv(ImageCy, Layout->ScaleNum, Layout->ScaleDen, &ScaledCy) ||
        ScaledCx <= 0 || ScaledCy <= 0)
        return FALSE;

    if (!SpanLongFrom64(((LONGLONG)ScaledCx - CanvasCx) * ImageCx /
                        (2 * (LONGLONG)ScaledCx), &Source.left) ||
        !SpanLongFrom64(((LONGLONG)ScaledCy - CanvasCy) * ImageCy /
                        (2 * (LONGLONG)ScaledCy), &Source.top))
        return FALSE;

    if (!SpanLongFrom64((LONGLONG)Source.left + CanvasCx * ImageCx / ScaledCx,
                        &Source.right) ||
        !SpanLongFrom64((LONGLONG)Source.top + CanvasCy * ImageCy / ScaledCy,
                        &Source.bottom))
        return FALSE;

    if (Source.right <= Source.left || Source.bottom <= Source.top)
        return FALSE;

    if (Source.right > ImageCx || Source.bottom > ImageCy)
        return FALSE;

    Layout->Source = Source;
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
    LONGLONG CanvasCx, CanvasCy, SourceCx, SourceCy;
    RECTL Clip;

    if (!Layout || !VirtualBounds || !Monitor || !Dest || !Source)
        return FALSE;

    CanvasCx = (LONGLONG)VirtualBounds->right - VirtualBounds->left;
    CanvasCy = (LONGLONG)VirtualBounds->bottom - VirtualBounds->top;
    if (ImageCx <= 0 || ImageCy <= 0 ||
        CanvasCx <= 0 || CanvasCx > LONG_MAX ||
        CanvasCy <= 0 || CanvasCy > LONG_MAX)
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

    if (Layout->Source.left < 0 || Layout->Source.top < 0 ||
        Layout->Source.right > ImageCx || Layout->Source.bottom > ImageCy)
        return FALSE;

    SourceCx = (LONGLONG)Layout->Source.right - Layout->Source.left;
    SourceCy = (LONGLONG)Layout->Source.bottom - Layout->Source.top;
    if (SourceCx < 0 || SourceCx > LONG_MAX ||
        SourceCy < 0 || SourceCy > LONG_MAX)
        return FALSE;

    *Dest = Clip;
    if (!SpanLongFrom64((LONGLONG)Layout->Source.left +
                        ((LONGLONG)Clip.left - VirtualBounds->left) * SourceCx / CanvasCx,
                        &Source->left) ||
        !SpanLongFrom64((LONGLONG)Layout->Source.left +
                        ((LONGLONG)Clip.right - VirtualBounds->left) * SourceCx / CanvasCx,
                        &Source->right) ||
        !SpanLongFrom64((LONGLONG)Layout->Source.top +
                        ((LONGLONG)Clip.top - VirtualBounds->top) * SourceCy / CanvasCy,
                        &Source->top) ||
        !SpanLongFrom64((LONGLONG)Layout->Source.top +
                        ((LONGLONG)Clip.bottom - VirtualBounds->top) * SourceCy / CanvasCy,
                        &Source->bottom))
        return FALSE;

    return TRUE;
}
