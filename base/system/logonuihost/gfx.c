/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Drawing layer: premultiplied surfaces, shapes, text, blur, layered present, timed animation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include "gfx.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

static LONG g_Dpi = 96;
static UINT g_RefreshHz = 60;

VOID
LuhGfxInit(VOID)
{
    HDC Screen = GetDC(NULL);
    int Value;

    if (Screen == NULL)
        return;
    Value = GetDeviceCaps(Screen, LOGPIXELSY);
    if (Value >= 96)
        g_Dpi = Value;
    /* 0 and 1 mean "the hardware default"; anything implausible keeps 60. */
    Value = GetDeviceCaps(Screen, VREFRESH);
    if (Value >= 30 && Value <= 240)
        g_RefreshHz = (UINT)Value;
    ReleaseDC(NULL, Screen);
}

LONG
LuhPx(
    _In_ LONG Design)
{
    return MulDiv(Design, g_Dpi, 96);
}

UINT
LuhRefreshHz(VOID)
{
    return g_RefreshHz;
}

BOOL
LuhSurfaceCreate(
    _Out_ PLUH_SURFACE Surface,
    _In_ LONG Width,
    _In_ LONG Height)
{
    BITMAPINFO Info;
    PVOID Bits = NULL;

    ZeroMemory(Surface, sizeof(*Surface));
    if (Width <= 0 || Height <= 0)
        return FALSE;

    ZeroMemory(&Info, sizeof(Info));
    Info.bmiHeader.biSize = sizeof(Info.bmiHeader);
    Info.bmiHeader.biWidth = Width;
    Info.bmiHeader.biHeight = -Height;
    Info.bmiHeader.biPlanes = 1;
    Info.bmiHeader.biBitCount = 32;
    Info.bmiHeader.biCompression = BI_RGB;

    Surface->Dc = CreateCompatibleDC(NULL);
    if (Surface->Dc == NULL)
        return FALSE;
    Surface->Bitmap = CreateDIBSection(Surface->Dc, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
    if (Surface->Bitmap == NULL || Bits == NULL)
    {
        ERR("CreateDIBSection %ldx%ld failed (%lu)\n", Width, Height, GetLastError());
        DeleteDC(Surface->Dc);
        ZeroMemory(Surface, sizeof(*Surface));
        return FALSE;
    }
    Surface->Old = SelectObject(Surface->Dc, Surface->Bitmap);
    Surface->Bits = Bits;
    Surface->Width = Width;
    Surface->Height = Height;
    return TRUE;
}

VOID
LuhSurfaceDestroy(
    _Inout_ PLUH_SURFACE Surface)
{
    if (Surface->Dc != NULL)
    {
        SelectObject(Surface->Dc, Surface->Old);
        DeleteDC(Surface->Dc);
    }
    if (Surface->Bitmap != NULL)
        DeleteObject(Surface->Bitmap);
    ZeroMemory(Surface, sizeof(*Surface));
}

VOID
LuhSurfaceClear(
    _Inout_ PLUH_SURFACE Surface)
{
    GdiFlush();
    ZeroMemory(Surface->Bits, (SIZE_T)Surface->Width * Surface->Height * sizeof(ULONG));
}

/* Paint Color at opacity A (0-255) over one premultiplied pixel. */
static __inline ULONG
LuhOver(
    _In_ ULONG Pixel,
    _In_ COLORREF Color,
    _In_ ULONG A)
{
    ULONG Keep = 255 - A;
    ULONG B = (GetBValue(Color) * A + ((Pixel & 0xFF) * Keep)) / 255;
    ULONG G = (GetGValue(Color) * A + (((Pixel >> 8) & 0xFF) * Keep)) / 255;
    ULONG R = (GetRValue(Color) * A + (((Pixel >> 16) & 0xFF) * Keep)) / 255;
    ULONG Alpha = A + (((Pixel >> 24) & 0xFF) * Keep) / 255;

    return (Alpha << 24) | (R << 16) | (G << 8) | B;
}

/* A premultiplied source pixel scaled by Opacity, over the target pixel. */
static __inline ULONG
LuhOverPremultiplied(
    _In_ ULONG Target,
    _In_ ULONG Source,
    _In_ ULONG Opacity)
{
    ULONG SA = (((Source >> 24) & 0xFF) * Opacity) / 255;
    ULONG Keep = 255 - SA;
    ULONG B = ((Source & 0xFF) * Opacity) / 255 + ((Target & 0xFF) * Keep) / 255;
    ULONG G = (((Source >> 8) & 0xFF) * Opacity) / 255 + (((Target >> 8) & 0xFF) * Keep) / 255;
    ULONG R = (((Source >> 16) & 0xFF) * Opacity) / 255 + (((Target >> 16) & 0xFF) * Keep) / 255;
    ULONG A = SA + (((Target >> 24) & 0xFF) * Keep) / 255;

    if (B > 255) B = 255;
    if (G > 255) G = 255;
    if (R > 255) R = 255;
    if (A > 255) A = 255;
    return (A << 24) | (R << 16) | (G << 8) | B;
}

static BOOL
LuhClip(
    _In_ const LUH_SURFACE *Surface,
    _In_ const RECT *Rect,
    _Out_ RECT *Clipped)
{
    RECT Bounds = { 0, 0, Surface->Width, Surface->Height };

    return IntersectRect(Clipped, Rect, &Bounds);
}

VOID
LuhFillRect(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Rect,
    _In_ COLORREF Color,
    _In_ BYTE Alpha)
{
    RECT Area;
    LONG X, Y;
    PULONG Row;

    if (Alpha == 0 || !LuhClip(Surface, Rect, &Area))
        return;
    GdiFlush();
    for (Y = Area.top; Y < Area.bottom; Y++)
    {
        Row = Surface->Bits + (SIZE_T)Y * Surface->Width;
        for (X = Area.left; X < Area.right; X++)
            Row[X] = LuhOver(Row[X], Color, Alpha);
    }
}

/*
 * Coverage of the pixel (X, Y) by a rounded rectangle, 0-255: 255 inside,
 * an analytic one-pixel ramp along the arc of each corner.
 */
static ULONG
LuhRoundCoverage(
    _In_ const RECT *Rect,
    _In_ LONG Radius,
    _In_ LONG X,
    _In_ LONG Y)
{
    double Cx, Cy, Px = X + 0.5, Py = Y + 0.5, Distance, Cover;

    if (X < Rect->left + Radius)
        Cx = Rect->left + Radius;
    else if (X >= Rect->right - Radius)
        Cx = Rect->right - Radius;
    else
        return 255;
    if (Y < Rect->top + Radius)
        Cy = Rect->top + Radius;
    else if (Y >= Rect->bottom - Radius)
        Cy = Rect->bottom - Radius;
    else
        return 255;

    Distance = sqrt((Px - Cx) * (Px - Cx) + (Py - Cy) * (Py - Cy));
    Cover = Radius + 0.5 - Distance;
    if (Cover <= 0.0)
        return 0;
    if (Cover >= 1.0)
        return 255;
    return (ULONG)(Cover * 255.0 + 0.5);
}

VOID
LuhFillRound(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Rect,
    _In_ LONG Radius,
    _In_ COLORREF Color,
    _In_ BYTE Alpha)
{
    RECT Area;
    LONG X, Y, Limit;
    PULONG Row;
    ULONG Cover;

    Limit = min(Rect->right - Rect->left, Rect->bottom - Rect->top) / 2;
    if (Radius > Limit)
        Radius = Limit;
    if (Radius <= 0)
    {
        LuhFillRect(Surface, Rect, Color, Alpha);
        return;
    }
    if (Alpha == 0 || !LuhClip(Surface, Rect, &Area))
        return;

    GdiFlush();
    for (Y = Area.top; Y < Area.bottom; Y++)
    {
        Row = Surface->Bits + (SIZE_T)Y * Surface->Width;
        for (X = Area.left; X < Area.right; X++)
        {
            Cover = LuhRoundCoverage(Rect, Radius, X, Y);
            if (Cover != 0)
                Row[X] = LuhOver(Row[X], Color, (Cover * Alpha) / 255);
        }
    }
}

VOID
LuhFrameRound(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Rect,
    _In_ LONG Radius,
    _In_ LONG Width,
    _In_ COLORREF Color,
    _In_ BYTE Alpha)
{
    RECT Area, Inner = *Rect;
    LONG X, Y, Limit, InnerRadius;
    PULONG Row;
    ULONG Outer, In, Cover;

    Limit = min(Rect->right - Rect->left, Rect->bottom - Rect->top) / 2;
    if (Radius > Limit)
        Radius = Limit;
    if (Alpha == 0 || Width <= 0 || !LuhClip(Surface, Rect, &Area))
        return;
    InflateRect(&Inner, -Width, -Width);
    InnerRadius = max(Radius - Width, 0);

    GdiFlush();
    for (Y = Area.top; Y < Area.bottom; Y++)
    {
        Row = Surface->Bits + (SIZE_T)Y * Surface->Width;
        for (X = Area.left; X < Area.right; X++)
        {
            Outer = Radius > 0 ? LuhRoundCoverage(Rect, Radius, X, Y) : 255;
            if (X < Inner.left || X >= Inner.right || Y < Inner.top || Y >= Inner.bottom)
                In = 0;
            else
                In = InnerRadius > 0 ? LuhRoundCoverage(&Inner, InnerRadius, X, Y) : 255;
            Cover = Outer > In ? Outer - In : 0;
            if (Cover != 0)
                Row[X] = LuhOver(Row[X], Color, (Cover * Alpha) / 255);
        }
    }
}

VOID
LuhFillCircle(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG CenterX,
    _In_ LONG CenterY,
    _In_ LONG Radius,
    _In_ COLORREF Color,
    _In_ BYTE Alpha)
{
    RECT Rect = { CenterX - Radius, CenterY - Radius, CenterX + Radius, CenterY + Radius };

    LuhFillRound(Surface, &Rect, Radius, Color, Alpha);
}

HFONT
LuhFontCreate(
    _In_ LONG DesignHeight,
    _In_ LONG Weight)
{
    LOGFONTW Font;
    NONCLIENTMETRICSW Metrics;

    ZeroMemory(&Font, sizeof(Font));
    Metrics.cbSize = sizeof(Metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0))
        StringCchCopyW(Font.lfFaceName, ARRAYSIZE(Font.lfFaceName), Metrics.lfMessageFont.lfFaceName);
    else
        StringCchCopyW(Font.lfFaceName, ARRAYSIZE(Font.lfFaceName), L"Segoe UI");
    Font.lfHeight = -LuhPx(DesignHeight);
    Font.lfWeight = Weight;
    Font.lfCharSet = DEFAULT_CHARSET;
    /* Grayscale coverage only: ClearType fringes cannot be blended over a layered surface. */
    Font.lfQuality = ANTIALIASED_QUALITY;
    return CreateFontIndirectW(&Font);
}

SIZE
LuhTextSize(
    _In_ HFONT Font,
    _In_ PCWSTR Text)
{
    SIZE Size = { 0, 0 };
    RECT Rect = { 0, 0, 0, 0 };
    HDC Dc = CreateCompatibleDC(NULL);
    HGDIOBJ Old;

    if (Dc == NULL)
        return Size;
    Old = SelectObject(Dc, Font);
    DrawTextW(Dc, Text, -1, &Rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(Dc, Old);
    DeleteDC(Dc);
    Size.cx = Rect.right - Rect.left;
    Size.cy = Rect.bottom - Rect.top;
    return Size;
}

/*
 * GDI text does not write alpha, so it is drawn white on black into a scratch
 * surface whose green channel then serves as the coverage of each pixel.
 */
VOID
LuhText(
    _Inout_ PLUH_SURFACE Surface,
    _In_ HFONT Font,
    _In_ PCWSTR Text,
    _In_ const RECT *Rect,
    _In_ UINT Format,
    _In_ COLORREF Color,
    _In_ BYTE Alpha)
{
    LUH_SURFACE Mask;
    RECT Local, Area;
    HGDIOBJ Old;
    LONG X, Y;
    PULONG Row, MaskRow;
    ULONG Cover;

    if (Alpha == 0 || Text == NULL || Text[0] == UNICODE_NULL || !LuhClip(Surface, Rect, &Area))
        return;
    if (!LuhSurfaceCreate(&Mask, Rect->right - Rect->left, Rect->bottom - Rect->top))
        return;

    SetRect(&Local, 0, 0, Mask.Width, Mask.Height);
    Old = SelectObject(Mask.Dc, Font);
    SetBkMode(Mask.Dc, TRANSPARENT);
    SetTextColor(Mask.Dc, RGB(255, 255, 255));
    DrawTextW(Mask.Dc, Text, -1, &Local, Format | DT_NOPREFIX);
    SelectObject(Mask.Dc, Old);
    GdiFlush();

    for (Y = Area.top; Y < Area.bottom; Y++)
    {
        Row = Surface->Bits + (SIZE_T)Y * Surface->Width;
        MaskRow = Mask.Bits + (SIZE_T)(Y - Rect->top) * Mask.Width;
        for (X = Area.left; X < Area.right; X++)
        {
            Cover = (MaskRow[X - Rect->left] >> 8) & 0xFF;
            if (Cover != 0)
                Row[X] = LuhOver(Row[X], Color, (Cover * Alpha) / 255);
        }
    }
    LuhSurfaceDestroy(&Mask);
}

BOOL
LuhSurfaceScaleFrom(
    _Inout_ PLUH_SURFACE Target,
    _In_ const LUH_SURFACE *Source)
{
    SIZE_T Count, i;
    BOOL Ok;

    SetStretchBltMode(Target->Dc, HALFTONE);
    SetBrushOrgEx(Target->Dc, 0, 0, NULL);
    Ok = StretchBlt(Target->Dc, 0, 0, Target->Width, Target->Height,
                    Source->Dc, 0, 0, Source->Width, Source->Height, SRCCOPY);
    GdiFlush();
    Count = (SIZE_T)Target->Width * Target->Height;
    for (i = 0; i < Count; i++)
        Target->Bits[i] |= 0xFF000000;
    return Ok;
}

/* One box pass along a line of Count pixels Step apart; Line holds a copy. */
static VOID
LuhBoxLine(
    _Inout_ PULONG Pixels,
    _In_ LONG Count,
    _In_ LONG Step,
    _In_ LONG Radius,
    _Inout_ PULONG Line)
{
    LONG i, Window = 2 * Radius + 1;
    ULONG Sum[4] = { 0, 0, 0, 0 }, P, Lo, Hi;
    int c;

    for (i = 0; i < Count; i++)
        Line[i] = Pixels[(SIZE_T)i * Step];

    /* The edges repeat the first and last pixel. */
    for (i = -Radius; i <= Radius; i++)
    {
        P = Line[min(max(i, 0), Count - 1)];
        for (c = 0; c < 4; c++)
            Sum[c] += (P >> (8 * c)) & 0xFF;
    }
    for (i = 0; i < Count; i++)
    {
        Pixels[(SIZE_T)i * Step] = ((Sum[3] / Window) << 24) | ((Sum[2] / Window) << 16) |
                                   ((Sum[1] / Window) << 8) | (Sum[0] / Window);
        Lo = Line[max(i - Radius, 0)];
        Hi = Line[min(i + Radius + 1, Count - 1)];
        for (c = 0; c < 4; c++)
            Sum[c] += ((Hi >> (8 * c)) & 0xFF) - ((Lo >> (8 * c)) & 0xFF);
    }
}

VOID
LuhBlur(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG Radius)
{
    PULONG Line;
    LONG X, Y, Pass;

    if (Radius <= 0)
        return;
    Line = HeapAlloc(GetProcessHeap(), 0, sizeof(ULONG) * max(Surface->Width, Surface->Height));
    if (Line == NULL)
        return;
    GdiFlush();
    for (Pass = 0; Pass < 3; Pass++)
    {
        for (Y = 0; Y < Surface->Height; Y++)
            LuhBoxLine(Surface->Bits + (SIZE_T)Y * Surface->Width, Surface->Width, 1, Radius, Line);
        for (X = 0; X < Surface->Width; X++)
            LuhBoxLine(Surface->Bits + X, Surface->Height, Surface->Width, Radius, Line);
    }
    HeapFree(GetProcessHeap(), 0, Line);
}

VOID
LuhBlend(
    _Inout_ PLUH_SURFACE Target,
    _In_ LONG X,
    _In_ LONG Y,
    _In_ const LUH_SURFACE *Source,
    _In_ BYTE Alpha)
{
    BLENDFUNCTION Blend = { AC_SRC_OVER, 0, Alpha, AC_SRC_ALPHA };

    if (Alpha == 0)
        return;
    GdiAlphaBlend(Target->Dc, X, Y, Source->Width, Source->Height,
                  Source->Dc, 0, 0, Source->Width, Source->Height, Blend);
    GdiFlush();
}

VOID
LuhBlendCircle(
    _Inout_ PLUH_SURFACE Target,
    _In_ LONG X,
    _In_ LONG Y,
    _In_ const LUH_SURFACE *Source)
{
    RECT Circle = { 0, 0, Source->Width, Source->Height };
    LONG Radius = min(Source->Width, Source->Height) / 2, Sx, Sy, Tx, Ty;
    ULONG Cover;

    GdiFlush();
    for (Sy = 0; Sy < Source->Height; Sy++)
    {
        Ty = Y + Sy;
        if (Ty < 0 || Ty >= Target->Height)
            continue;
        for (Sx = 0; Sx < Source->Width; Sx++)
        {
            Tx = X + Sx;
            if (Tx < 0 || Tx >= Target->Width)
                continue;
            Cover = LuhRoundCoverage(&Circle, Radius, Sx, Sy);
            if (Cover != 0)
            {
                PULONG Pixel = Target->Bits + (SIZE_T)Ty * Target->Width + Tx;
                *Pixel = LuhOverPremultiplied(*Pixel, Source->Bits[(SIZE_T)Sy * Source->Width + Sx], Cover);
            }
        }
    }
}

BOOL
LuhPresent(
    _In_ HWND Window,
    _In_ const LUH_SURFACE *Surface,
    _In_ BYTE Alpha)
{
    BLENDFUNCTION Blend = { AC_SRC_OVER, 0, Alpha, AC_SRC_ALPHA };
    SIZE Size = { Surface->Width, Surface->Height };
    POINT Origin = { 0, 0 };

    GdiFlush();
    return UpdateLayeredWindow(Window, NULL, NULL, &Size, Surface->Dc, &Origin, 0, &Blend, ULW_ALPHA);
}

static LONGLONG
LuhNow(VOID)
{
    LARGE_INTEGER Count;

    QueryPerformanceCounter(&Count);
    return Count.QuadPart;
}

VOID
LuhAnimStart(
    _Out_ PLUH_ANIM Anim,
    _In_ PCSTR Name,
    _In_ DWORD DurationMs)
{
    LARGE_INTEGER Frequency;

    ZeroMemory(Anim, sizeof(*Anim));
    if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart == 0)
        Frequency.QuadPart = 1000;
    Anim->Name = Name;
    Anim->Frequency = Frequency.QuadPart;
    Anim->Target = Frequency.QuadPart / LuhRefreshHz();
    Anim->DurationMs = DurationMs;
    Anim->Start = LuhNow();
    Anim->Last = Anim->Start;
    Anim->Next = Anim->Start;
}

double
LuhAnimFrame(
    _Inout_ PLUH_ANIM Anim)
{
    LONGLONG Now = LuhNow(), Interval = Now - Anim->Last;
    double T;

    if (Anim->Frames > 0)
    {
        if (Interval > Anim->Longest)
            Anim->Longest = Interval;
        if (Interval > 2 * Anim->Target)
            Anim->Late++;
    }
    Anim->Frames++;
    Anim->Last = Now;

    if (Anim->DurationMs == 0)
        return 1.0;
    T = (double)(Now - Anim->Start) * 1000.0 / ((double)Anim->Frequency * Anim->DurationMs);
    return T >= 1.0 ? 1.0 : T;
}

BOOL
LuhAnimDone(
    _In_ const LUH_ANIM *Anim)
{
    return Anim->DurationMs != 0 &&
           (Anim->Last - Anim->Start) * 1000 >= (LONGLONG)Anim->DurationMs * Anim->Frequency;
}

BOOL
LuhAnimWait(
    _Inout_ PLUH_ANIM Anim)
{
    LONGLONG Now;
    DWORD Ms;

    /* Deadlines follow a fixed schedule from the start, so a late frame does not push the rest. */
    Anim->Next += Anim->Target;
    Now = LuhNow();
    if (Anim->Next <= Now)
    {
        Anim->Next = Now;
        return FALSE;
    }
    Ms = (DWORD)(((Anim->Next - Now) * 1000) / Anim->Frequency);
    if (Ms == 0)
        return FALSE;
    return MsgWaitForMultipleObjectsEx(0, NULL, Ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0;
}

VOID
LuhAnimEnd(
    _Inout_ PLUH_ANIM Anim)
{
    CHAR Line[128];

    /* Pam's harness reads this line: a late frame is an interval over twice the target. */
    StringCchPrintfA(Line, ARRAYSIZE(Line), "anim %s frames=%lu longest-ms=%lu late=%lu target-ms=%lu",
                     Anim->Name ? Anim->Name : "?", Anim->Frames,
                     (ULONG)((Anim->Longest * 1000) / Anim->Frequency), Anim->Late,
                     (ULONG)((Anim->Target * 1000) / Anim->Frequency));
    LuhStage(Line);
}

double
LuhEaseOut(
    _In_ double T)
{
    double U = 1.0 - T;

    return 1.0 - U * U * U;
}

double
LuhEaseInOut(
    _In_ double T)
{
    double U;

    if (T < 0.5)
        return 4.0 * T * T * T;
    U = -2.0 * T + 2.0;
    return 1.0 - U * U * U / 2.0;
}

LONG
LuhLerp(
    _In_ LONG From,
    _In_ LONG To,
    _In_ double T)
{
    return From + (LONG)floor((To - From) * T + 0.5);
}
