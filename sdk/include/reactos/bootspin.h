/*
 * PROJECT:     LiberNT
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Boot spinner geometry and animation, shared by the kernel (ntoskrnl/inbv)
 *              and the logon UI host (winlogon) so the spinner the kernel draws during boot
 *              and the one winlogon continues after the hand-over are laid out and animated
 *              by one piece of code. Pure integer math, no allocation, no kernel- or user-only
 *              dependency, so the same header compiles in both.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#ifndef _LIBERNT_BOOTSPIN_H
#define _LIBERNT_BOOTSPIN_H

#pragma once

/*
 * The spinner's phase is the interrupt time itself (100 ns since boot): the kernel renders at
 * KeQueryInterruptTime() and hands the display over at the current phase; winlogon renders at
 * the same clock read from the shared user page, so the animation continues without a jump.
 */

/* The DPI rule: boot framebuffer DPI, clamped, scales the 96-DPI design sizes below.
 * Mirrors sdk/include/reactos/loader_framebuffer.h; redefined here only if that is absent. */
#ifndef LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT
#define LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT 96
#define LOADER_PARAMETER_FRAMEBUFFER_DPI_MIN     96
#define LOADER_PARAMETER_FRAMEBUFFER_DPI_MAX     480
#endif

/* Where the spinner sits: the centre of the 51 x 45 area the previous mark used (96-DPI px) */
#define INBV_SPIN_LOGO_WIDTH        51
#define INBV_SPIN_LOGO_TOP_WIDTH    64
#define INBV_SPIN_LOGO_LIFT         20
#define INBV_SPIN_LOGO_UNITS        407
#define INBV_SPIN_LOGO_UNITS_HIGH   356
/* Vertical placement of the centre mark: 0.625 of the way down the lower half; 0.625 == 5/8. */
#define INBV_SPIN_CENTERMARK_NUM    5
#define INBV_SPIN_CENTERMARK_DEN    8

/* Frame cadence (ms) and the clock unit (100 ns) */
#define INBV_SPIN_FRAME_MS          15

/* Double-stroke breathing spinner, sizes in pixels at 96 DPI */
#define INBV_SPIN_SIZE              32
#define INBV_SPIN_INSET             7
#define INBV_SPIN_SUB               4            /* 4 x 4 sub-pixel samples */
#define INBV_SPIN_TURN              65536        /* one turn in angle units */
#define INBV_SPIN_OUTER_TURN        22000000ULL  /* 2.2 s clockwise, in 100 ns */
#define INBV_SPIN_INNER_TURN        28000000ULL  /* 2.8 s counterclockwise */
#define INBV_SPIN_BREATH            26000000ULL  /* 2.6 s, eased each half */
#define INBV_SPIN_OUTER_MIN         45
#define INBV_SPIN_OUTER_MAX         285
#define INBV_SPIN_INNER_MAX         250
#define INBV_SPIN_INNER_MIN         50
#define INBV_SPIN_INNER_ALPHA       166          /* 0.65 */
#define INBV_SPIN_EASE_STEPS        1024

/* The stroke colour (light grey), as 8-bit channels; each side packs it for its surface. */
#define INBV_SPIN_RED               0xE0
#define INBV_SPIN_GREEN             0xE0
#define INBV_SPIN_BLUE              0xE0

/* One sub-pixel sample on a ring: its angle clockwise from the top, the ring's opacity there,
 * and which ring (0 outer, 1 inner) it belongs to. */
typedef struct _INBV_SPIN_SAMPLE
{
    unsigned short Angle;
    unsigned char Alpha;
    unsigned char Inner;
} INBV_SPIN_SAMPLE, *PINBV_SPIN_SAMPLE;

/* The spinner's pixel rectangle on a ScreenW x ScreenH surface. Width == 0 means it does not fit. */
typedef struct _INBV_SPIN_RECT
{
    unsigned long Left;
    unsigned long Top;
    unsigned long Width;
    unsigned long Height;
} INBV_SPIN_RECT;

/* Internal linkage + an inline hint: one copy per translation unit, no external symbol, and no
 * unused-function warning if a unit does not reach every helper. Not force-inlined, so the larger
 * one-shot builders (the ease table) do not draw the MSVC "could not inline" warning. */
#ifndef INBV_SPIN_FORCEINLINE
#if defined(_MSC_VER)
#define INBV_SPIN_FORCEINLINE static __inline
#elif defined(__GNUC__)
#define INBV_SPIN_FORCEINLINE static __inline__
#else
#define INBV_SPIN_FORCEINLINE static __inline
#endif
#endif

/* ---- DPI rule ------------------------------------------------------------------------- */

INBV_SPIN_FORCEINLINE unsigned long
InbvSpinNormalizeDpi(unsigned long Dpi)
{
    if (Dpi < LOADER_PARAMETER_FRAMEBUFFER_DPI_MIN || Dpi > LOADER_PARAMETER_FRAMEBUFFER_DPI_MAX)
        return LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT;
    return Dpi;
}

INBV_SPIN_FORCEINLINE unsigned long
InbvSpinScaleForDpi(unsigned long Value, unsigned long Dpi)
{
    return (unsigned long)(((unsigned long long)Value * InbvSpinNormalizeDpi(Dpi) +
                            LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT / 2) /
                           LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT);
}

/* ---- Where the spinner sits ----------------------------------------------------------- */

/* Height of the 51 x 45 area the previous mark used: the spinner takes its centre and the kernel's
 * status line goes below it. */
INBV_SPIN_FORCEINLINE unsigned long
InbvSpinMarkHeight(unsigned long Dpi)
{
    unsigned long MarkWidth = InbvSpinScaleForDpi(INBV_SPIN_LOGO_WIDTH, Dpi);

    return (INBV_SPIN_LOGO_UNITS_HIGH * MarkWidth + INBV_SPIN_LOGO_UNITS / 2) / INBV_SPIN_LOGO_UNITS;
}

/* Top of that area on a surface ScreenH high. */
INBV_SPIN_FORCEINLINE unsigned long
InbvSpinMarkTop(unsigned long ScreenH, unsigned long Dpi)
{
    unsigned long MarkHeight = InbvSpinMarkHeight(Dpi);
    unsigned long HalfHeight = ScreenH / 2;
    unsigned long CenterY = HalfHeight + (ScreenH - HalfHeight) * INBV_SPIN_CENTERMARK_NUM / INBV_SPIN_CENTERMARK_DEN;
    unsigned long TopHalf = (unsigned long)(((unsigned long long)MarkHeight * INBV_SPIN_LOGO_TOP_WIDTH) /
                                            (2 * INBV_SPIN_LOGO_WIDTH));
    unsigned long Lift = InbvSpinScaleForDpi(INBV_SPIN_LOGO_LIFT, Dpi);

    return (CenterY > TopHalf + Lift) ? (CenterY - TopHalf - Lift) : 0;
}

/* The spinner rectangle for a ScreenW x ScreenH surface at the given DPI. Both the kernel
 * (boot framebuffer) and winlogon (desktop, which is the boot mode on an equal-size first
 * commit) compute it here, so the winlogon window lands on the kernel's pixels. */
INBV_SPIN_FORCEINLINE INBV_SPIN_RECT
InbvSpinComputeRect(unsigned long ScreenW, unsigned long ScreenH, unsigned long Dpi)
{
    INBV_SPIN_RECT Rect;
    unsigned long Size = InbvSpinScaleForDpi(INBV_SPIN_SIZE, Dpi);
    unsigned long MarkHeight = InbvSpinMarkHeight(Dpi);
    unsigned long MarkTop, Floor;

    Rect.Left = Rect.Top = Rect.Width = Rect.Height = 0;
    if (Size == 0 || Size > ScreenW || Size > ScreenH || MarkHeight > ScreenH)
        return Rect;

    MarkTop = InbvSpinMarkTop(ScreenH, Dpi);
    Floor = (MarkHeight + 1 < Size) ? (MarkHeight + 1) : Size;
    Rect.Left = (ScreenW - Size) / 2;
    Rect.Top = MarkTop + (MarkHeight + 1 - Floor) / 2;
    if (Rect.Top + Size > ScreenH)
        Rect.Top = ScreenH - Size;
    Rect.Width = Size;
    Rect.Height = Size;
    return Rect;
}

/* ---- Pure math used to build the rings and sweep them ---------------------------------- */

INBV_SPIN_FORCEINLINE unsigned long
InbvSpinSqrt(unsigned long long Value)
{
    unsigned long long Root = 0, Bit = 1ULL << 62;

    while (Bit > Value)
        Bit >>= 2;
    while (Bit)
    {
        if (Value >= Root + Bit)
        {
            Value -= Root + Bit;
            Root = (Root >> 1) + Bit;
        }
        else
        {
            Root >>= 1;
        }
        Bit >>= 2;
    }
    return (unsigned long)Root;
}

/* Angle of (Right, Up) clockwise from up, in turn units, by CORDIC */
INBV_SPIN_FORCEINLINE unsigned short
InbvSpinAngle(long Right, long Up)
{
    static const long Atan[] = { 8192, 4836, 2555, 1297, 651, 326, 163, 81, 41, 20, 10, 5, 3, 1 };
    long X = Up, Y = Right, Z = 0, NextX, i;

    if (X < 0)
    {
        X = -X;
        Y = -Y;
        Z = INBV_SPIN_TURN / 2;
    }
    X *= 64;
    Y *= 64;
    for (i = 0; i < (long)(sizeof(Atan) / sizeof(Atan[0])); i++)
    {
        if (Y > 0)
        {
            NextX = X + (Y >> i);
            Y -= X >> i;
            Z += Atan[i];
        }
        else
        {
            NextX = X - (Y >> i);
            Y += X >> i;
            Z -= Atan[i];
        }
        X = NextX;
    }
    return (unsigned short)(Z & (INBV_SPIN_TURN - 1));
}

/*
 * Opacity (0..255) of a 2 px ring of outer radius Radius at Distance from its centre, both in
 * 1/256 px with Unit = 1 px: rises from Radius - 2 to Radius - 1.5, falls from Radius - 0.5.
 */
INBV_SPIN_FORCEINLINE unsigned char
InbvSpinRing(long Distance, long Radius, long Unit)
{
    long Start = Radius - 2 * Unit, Full = Radius - Unit * 3 / 2, End = Radius - Unit / 2;

    if (Distance <= Start || Distance >= Radius)
        return 0;
    if (Distance < Full)
        return (unsigned char)((Distance - Start) * 255 / (Full - Start));
    if (Distance <= End)
        return 255;
    return (unsigned char)((Radius - Distance) * 255 / (Radius - End));
}

/* cubic-bezier(0.42, 0, 0.58, 1), the CSS ease-in-out of the owner's preview, as y for x steps.
 * Fills Ease[0 .. INBV_SPIN_EASE_STEPS]. */
INBV_SPIN_FORCEINLINE void
InbvSpinEaseTable(unsigned long *Ease)
{
    unsigned long Step;

    for (Step = 0; Step <= INBV_SPIN_EASE_STEPS; Step++)
    {
        unsigned long long Target = ((unsigned long long)Step << 48) / INBV_SPIN_EASE_STEPS;
        unsigned long long Low = 0, High = 65536, P, Q, X, Y;

        while (High - Low > 1)
        {
            P = (Low + High) / 2;
            Q = 65536 - P;
            X = (126 * Q * Q * P + 174 * Q * P * P) / 100 + P * P * P;
            if (X < Target)
                Low = P;
            else
                High = P;
        }
        P = High;
        Q = 65536 - P;
        Y = (3 * Q * P * P + P * P * P) >> 32;
        Ease[Step] = (unsigned long)(Y < 65536ULL ? Y : 65536ULL);
    }
}

/* Eased fraction (0..65536) of one half breath, at Position 0..Half */
INBV_SPIN_FORCEINLINE unsigned long
InbvSpinEase(const unsigned long *Ease, unsigned long long Position, unsigned long long Half)
{
    unsigned long long Scaled = Position * INBV_SPIN_EASE_STEPS * 256 / Half;
    unsigned long Index = (unsigned long)(Scaled >> 8), Fraction = (unsigned long)(Scaled & 0xFF);

    if (Index >= INBV_SPIN_EASE_STEPS)
        return Ease[INBV_SPIN_EASE_STEPS];
    return Ease[Index] + (((Ease[Index + 1] - Ease[Index]) * Fraction) >> 8);
}

/* Sweep in turn units at time T: From at both ends of the breath, To at its middle */
INBV_SPIN_FORCEINLINE unsigned long
InbvSpinSweep(const unsigned long *Ease, unsigned long long T, long From, long To)
{
    const unsigned long long Half = INBV_SPIN_BREATH / 2;
    unsigned long long Phase = T % INBV_SPIN_BREATH;
    long Degrees256;

    if (Phase < Half)
        Degrees256 = From * 256 + (long)(((long long)(To - From) * 256 * InbvSpinEase(Ease, Phase, Half)) >> 16);
    else
        Degrees256 = To * 256 + (long)(((long long)(From - To) * 256 * InbvSpinEase(Ease, Phase - Half, Half)) >> 16);
    return (unsigned long)(((long long)Degrees256 * INBV_SPIN_TURN) / (360 * 256));
}

/*
 * Build the sub-pixel samples for a Width x Height spinner. Two passes: with Samples == NULL it
 * only counts (fills PixelStart and returns the count), so the caller can size the Samples array;
 * with Samples it fills them. PixelStart has Width*Height + 1 entries (start index per pixel, plus
 * the total). Returns the number of samples.
 */
INBV_SPIN_FORCEINLINE unsigned long
InbvSpinBuildSamples(unsigned long Width, unsigned long Height,
                     INBV_SPIN_SAMPLE *Samples, unsigned long *PixelStart)
{
    unsigned long Count = Width * Height, Pixel, x, y, sx, sy, Next = 0;
    long Unit = (long)Width * 256 / INBV_SPIN_SIZE;
    long Center = (long)Width * 128;
    long Outer = Center;
    long Inner = Center - INBV_SPIN_INSET * Unit;

    for (Pixel = 0, y = 0; y < Height; y++)
    {
        for (x = 0; x < Width; x++, Pixel++)
        {
            PixelStart[Pixel] = Next;
            for (sy = 0; sy < INBV_SPIN_SUB; sy++)
            {
                for (sx = 0; sx < INBV_SPIN_SUB; sx++)
                {
                    long Dx = (long)(x * 256 + (sx * 256 + 128) / INBV_SPIN_SUB) - Center;
                    long Dy = (long)(y * 256 + (sy * 256 + 128) / INBV_SPIN_SUB) - Center;
                    long Distance = (long)InbvSpinSqrt((unsigned long long)((long long)Dx * Dx +
                                                                            (long long)Dy * Dy));
                    unsigned char OuterAlpha = InbvSpinRing(Distance, Outer, Unit);
                    unsigned char InnerAlpha = InbvSpinRing(Distance, Inner, Unit);

                    if (!OuterAlpha && !InnerAlpha)
                        continue;
                    if (Samples)
                    {
                        Samples[Next].Angle = InbvSpinAngle(Dx, -Dy);
                        Samples[Next].Inner = OuterAlpha ? 0 : 1;
                        Samples[Next].Alpha = OuterAlpha ? OuterAlpha :
                                              (unsigned char)(InnerAlpha * INBV_SPIN_INNER_ALPHA / 255);
                    }
                    Next++;
                }
            }
        }
    }
    PixelStart[Count] = Next;
    return Next;
}

/*
 * Render one frame at interrupt time T (100 ns since boot) into Out[0 .. Count-1], mapping each
 * pixel's stroke opacity (0..255) through Lut (256 entries, opacity -> the surface's packed colour)
 * that the caller fills for its own pixel format. Count is Width * Height. One pass, no allocation.
 */
INBV_SPIN_FORCEINLINE void
InbvSpinRenderFrame(const INBV_SPIN_SAMPLE *Samples, const unsigned long *PixelStart,
                    unsigned long Count, const unsigned long *Ease, unsigned long long T,
                    const unsigned long *Lut, unsigned long *Out)
{
    unsigned long Rotation[2], Sweep[2], Pixel, i;

    Rotation[0] = (unsigned long)((T % INBV_SPIN_OUTER_TURN) * INBV_SPIN_TURN / INBV_SPIN_OUTER_TURN);
    Rotation[1] = (INBV_SPIN_TURN - (unsigned long)((T % INBV_SPIN_INNER_TURN) * INBV_SPIN_TURN /
                  INBV_SPIN_INNER_TURN)) % INBV_SPIN_TURN;
    Sweep[0] = InbvSpinSweep(Ease, T, INBV_SPIN_OUTER_MIN, INBV_SPIN_OUTER_MAX);
    Sweep[1] = InbvSpinSweep(Ease, T, INBV_SPIN_INNER_MAX, INBV_SPIN_INNER_MIN);

    for (Pixel = 0; Pixel < Count; Pixel++)
    {
        unsigned long Sum = 0;

        for (i = PixelStart[Pixel]; i < PixelStart[Pixel + 1]; i++)
        {
            const INBV_SPIN_SAMPLE *Sample = &Samples[i];

            if (((Sample->Angle - Rotation[Sample->Inner]) & (INBV_SPIN_TURN - 1)) < Sweep[Sample->Inner])
                Sum += Sample->Alpha;
        }
        Sum /= INBV_SPIN_SUB * INBV_SPIN_SUB;
        Out[Pixel] = Lut[Sum < 255 ? Sum : 255];
    }
}

#endif /* _LIBERNT_BOOTSPIN_H */
