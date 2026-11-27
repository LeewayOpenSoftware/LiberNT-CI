/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Drawing layer: premultiplied surfaces, shapes, text, blur, layered present, timed animation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/*
 * Every screen is drawn once into a 32 bpp top-down DIB whose pixels hold
 * premultiplied BGRA. Animation never redraws a page: it moves or blends the
 * finished surfaces and hands the result to UpdateLayeredWindow.
 */
typedef struct _LUH_SURFACE
{
    HDC Dc;
    HBITMAP Bitmap;
    HGDIOBJ Old;
    PULONG Bits;
    LONG Width;
    LONG Height;
} LUH_SURFACE, *PLUH_SURFACE;

/* Sizes are designed at 96 DPI and scaled with LuhPx. */
VOID LuhGfxInit(VOID);
LONG LuhPx(_In_ LONG Design);
UINT LuhRefreshHz(VOID);

BOOL LuhSurfaceCreate(_Out_ PLUH_SURFACE Surface, _In_ LONG Width, _In_ LONG Height);
VOID LuhSurfaceDestroy(_Inout_ PLUH_SURFACE Surface);
VOID LuhSurfaceClear(_Inout_ PLUH_SURFACE Surface);

/* Colour is plain RGB; Alpha 0-255 is the opacity of the paint. */
VOID LuhFillRect(_Inout_ PLUH_SURFACE Surface, _In_ const RECT *Rect, _In_ COLORREF Color, _In_ BYTE Alpha);
VOID LuhFillRound(_Inout_ PLUH_SURFACE Surface, _In_ const RECT *Rect, _In_ LONG Radius,
                  _In_ COLORREF Color, _In_ BYTE Alpha);
VOID LuhFrameRound(_Inout_ PLUH_SURFACE Surface, _In_ const RECT *Rect, _In_ LONG Radius, _In_ LONG Width,
                   _In_ COLORREF Color, _In_ BYTE Alpha);
VOID LuhFillCircle(_Inout_ PLUH_SURFACE Surface, _In_ LONG CenterX, _In_ LONG CenterY, _In_ LONG Radius,
                   _In_ COLORREF Color, _In_ BYTE Alpha);

HFONT LuhFontCreate(_In_ LONG DesignHeight, _In_ LONG Weight);
/* Grayscale-antialiased text, blended with its own coverage; DrawText Format flags. */
VOID LuhText(_Inout_ PLUH_SURFACE Surface, _In_ HFONT Font, _In_ PCWSTR Text, _In_ const RECT *Rect,
             _In_ UINT Format, _In_ COLORREF Color, _In_ BYTE Alpha);
SIZE LuhTextSize(_In_ HFONT Font, _In_ PCWSTR Text);

/* Copy Source scaled onto the whole of Target (halftone), then make Target opaque. */
BOOL LuhSurfaceScaleFrom(_Inout_ PLUH_SURFACE Target, _In_ const LUH_SURFACE *Source);
/* Three box passes approximate a Gaussian; run once per backdrop, never per frame. */
VOID LuhBlur(_Inout_ PLUH_SURFACE Surface, _In_ LONG Radius);
/* Source over Target at (X, Y) with an extra constant opacity. */
VOID LuhBlend(_Inout_ PLUH_SURFACE Target, _In_ LONG X, _In_ LONG Y, _In_ const LUH_SURFACE *Source,
              _In_ BYTE Alpha);
/* Clip a circle out of Source (avatar): Target gets Source inside the circle only. */
VOID LuhBlendCircle(_Inout_ PLUH_SURFACE Target, _In_ LONG X, _In_ LONG Y, _In_ const LUH_SURFACE *Source);

BOOL LuhPresent(_In_ HWND Window, _In_ const LUH_SURFACE *Surface, _In_ BYTE Alpha);

/*
 * Animation follows the clock: the caller asks for the progress at "now", so a
 * late frame never slows the motion down. Every run ends with one trace line
 * (frames, longest interval, frames over twice the target) for the lab gate.
 */
typedef struct _LUH_ANIM
{
    PCSTR Name;
    LONGLONG Frequency;
    LONGLONG Start;
    LONGLONG Last;
    LONGLONG Next;
    LONGLONG Target;            /* one frame at the display rate, in counter ticks */
    DWORD DurationMs;
    DWORD Frames;
    DWORD Late;
    LONGLONG Longest;
} LUH_ANIM, *PLUH_ANIM;

VOID LuhAnimStart(_Out_ PLUH_ANIM Anim, _In_ PCSTR Name, _In_ DWORD DurationMs);
/* Records a frame and returns the linear progress 0..1 at this instant. */
double LuhAnimFrame(_Inout_ PLUH_ANIM Anim);
BOOL LuhAnimDone(_In_ const LUH_ANIM *Anim);
/* Sleeps to the next frame deadline; returns early (TRUE) when input is waiting. */
BOOL LuhAnimWait(_Inout_ PLUH_ANIM Anim);
VOID LuhAnimEnd(_Inout_ PLUH_ANIM Anim);

double LuhEaseOut(_In_ double T);
double LuhEaseInOut(_In_ double T);
LONG LuhLerp(_In_ LONG From, _In_ LONG To, _In_ double T);
