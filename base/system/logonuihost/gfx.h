/*
 * PROJECT:     LiberNT LogonUI drawing layer (logonuihost, SlideToShutDown)
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

/* Each program provides this: one LOGONSTAGE line (the animators' frame lines go there). */
VOID LuhStage(_In_ PCSTR Line);

/* Sizes are designed at 96 DPI and scaled with LuhPx. */
VOID LuhGfxInit(VOID);
VOID LuhGfxShutdown(VOID);
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

/* A disc with a second disc taken out of it (a crescent), by coverage: no colour needed for the cut. */
VOID LuhFillCrescent(_Inout_ PLUH_SURFACE Surface, _In_ double Cx, _In_ double Cy, _In_ double R,
                     _In_ double CutX, _In_ double CutY, _In_ double CutR, _In_ COLORREF Color, _In_ BYTE Alpha);
VOID LuhStroke(_Inout_ PLUH_SURFACE Surface, _In_ double X0, _In_ double Y0, _In_ double X1, _In_ double Y1,
               _In_ double Width, _In_ COLORREF Color, _In_ BYTE Alpha);

HFONT LuhFontCreate(_In_ LONG DesignHeight, _In_ LONG Weight);
/* Grayscale-antialiased text, blended with its own coverage; DrawText Format flags. */
VOID LuhDrawText(_Inout_ PLUH_SURFACE Surface, _In_ HFONT Font, _In_ PCWSTR Text, _In_ const RECT *Rect,
             _In_ UINT Format, _In_ COLORREF Color, _In_ BYTE Alpha);
SIZE LuhMeasureText(_In_ HFONT Font, _In_ PCWSTR Text);

/* Decode an open image file (WIC) into a new surface, scaled down until it just covers MaxWidth x MaxHeight. */
BOOL LuhSurfaceLoad(_Out_ PLUH_SURFACE Surface, _In_ HANDLE File, _In_ LONG MaxWidth, _In_ LONG MaxHeight);
/*
 * Fill the whole of Target with Source (halftone), cropping Source to Target's aspect.
 * For opaque pictures only (wallpaper, backdrop): every target pixel ends with alpha 0xFF.
 */
BOOL LuhSurfaceScaleFrom(_Inout_ PLUH_SURFACE Target, _In_ const LUH_SURFACE *Source);
/*
 * The soft backdrop behind the credentials: Target becomes Sharp, softly blurred. Run once per
 * show, never per frame. The blur lives here alone, so a better one replaces only this body.
 * FALSE when it could not run; the caller then shows Sharp as it is.
 */
BOOL LuhSoften(_Inout_ PLUH_SURFACE Target, _In_ const LUH_SURFACE *Sharp);
/* Source over Target at (X, Y) with an extra constant opacity. */
VOID LuhBlend(_Inout_ PLUH_SURFACE Target, _In_ LONG X, _In_ LONG Y, _In_ const LUH_SURFACE *Source,
              _In_ BYTE Alpha);
/* Clip a circle out of Source (avatar): the largest circle centred in Source, over Target. */
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
    BOOL FineTimer;             /* timeBeginPeriod(1) is held for this run */
    DWORD DurationMs;
    DWORD Frames;
    DWORD Late;
    LONGLONG Longest;
} LUH_ANIM, *PLUH_ANIM;

VOID LuhAnimStart(_Out_ PLUH_ANIM Anim, _In_ PCSTR Name, _In_ DWORD DurationMs);
/* Records a frame and returns the linear progress 0..1 at this instant. */
double LuhAnimFrame(_Inout_ PLUH_ANIM Anim);
BOOL LuhAnimDone(_In_ const LUH_ANIM *Anim);
double LuhAnimElapsedMs(_In_ const LUH_ANIM *Anim);
/* Sleeps to the next frame deadline of the fixed schedule. */
VOID LuhAnimWait(_Inout_ PLUH_ANIM Anim);
VOID LuhAnimEnd(_Inout_ PLUH_ANIM Anim);

double LuhEaseOut(_In_ double T);
double LuhEaseInOut(_In_ double T);
LONG LuhLerp(_In_ LONG From, _In_ LONG To, _In_ double T);
