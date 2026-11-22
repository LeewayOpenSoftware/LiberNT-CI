/*
 * PROJECT:     LiberNT
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Display scale list and recommendation shared by win32k and Settings
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* The Windows DEVICE_SCALE_FACTOR values; the steps widen above 250% */
static const unsigned int DpiScaleStandard[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

#define DPI_SCALE_COUNT             (sizeof(DpiScaleStandard) / sizeof(DpiScaleStandard[0]))
#define DPI_SCALE_MIN               100
#define DPI_SCALE_MAX               500

/* A scale is offered while the desktop keeps at least 1024x576 effective pixels */
#define DPI_SCALE_OFFER_MIN_WIDTH   1024
#define DPI_SCALE_OFFER_MIN_HEIGHT  576

/* A portable panel is recommended the largest scale keeping 1280x1000 effective pixels */
#define DPI_SCALE_RECOMMEND_MIN_WIDTH  1280
#define DPI_SCALE_RECOMMEND_MIN_HEIGHT 1000

static __inline int
DpiScaleFits(unsigned long Width, unsigned long Height, unsigned int Percent,
             unsigned long MinWidth, unsigned long MinHeight)
{
    return Percent >= DPI_SCALE_MIN &&
           (unsigned long long)Width * 100 / Percent >= MinWidth &&
           (unsigned long long)Height * 100 / Percent >= MinHeight;
}

static __inline int
DpiScaleIsOffered(unsigned long Width, unsigned long Height, unsigned int Percent)
{
    if (Percent == DPI_SCALE_MIN)
        return 1;
    return DpiScaleFits(Width, Height, Percent, DPI_SCALE_OFFER_MIN_WIDTH, DPI_SCALE_OFFER_MIN_HEIGHT);
}

/* No physical size (EDID) yet: a portable chassis gets the effective-size rule, anything else 100% */
static __inline unsigned int
DpiScaleRecommended(unsigned long Width, unsigned long Height, int Portable)
{
    unsigned int Index, Scale = DPI_SCALE_MIN;

    if (!Portable || !Width || !Height)
        return Scale;
    for (Index = 0; Index < DPI_SCALE_COUNT; ++Index)
    {
        if (!DpiScaleFits(Width, Height, DpiScaleStandard[Index],
                          DPI_SCALE_RECOMMEND_MIN_WIDTH, DPI_SCALE_RECOMMEND_MIN_HEIGHT))
            break;
        Scale = DpiScaleStandard[Index];
    }
    return Scale;
}

/* SMBIOS type 3 chassis types Windows treats as portable */
static __inline int
DpiScaleIsPortableChassis(unsigned char Type)
{
    switch (Type & 0x7F)
    {
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0E: case 0x1E: case 0x1F: case 0x20:
            return 1;
    }
    return 0;
}
