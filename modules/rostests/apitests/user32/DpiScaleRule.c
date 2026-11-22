/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Checks the offered and recommended display scales of dpiscale.h against Windows 11
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <dpiscale.h>

typedef struct _SCALE_CASE
{
    ULONG Width;
    ULONG Height;
    UINT LargestOffered;
    UINT RecommendedPortable;
} SCALE_CASE;

/* Largest offered scale as Windows 11 lists it; the portable recommendation is the effective-size rule
 * until the physical panel size (EDID) is known */
static const SCALE_CASE Cases[] =
{
    { 1280,  800, 125, 100 },
    { 1366,  768, 125, 100 },
    { 1920, 1080, 175, 100 },
    { 1920, 1200, 175, 100 },
    { 2560, 1440, 250, 125 },
    { 2560, 1600, 250, 150 },
    { 3840, 2160, 350, 200 },
};

START_TEST(DpiScaleRule)
{
    UINT i, j, largest, previous;

    ok(DPI_SCALE_COUNT == 12, "DPI_SCALE_COUNT = %u\n", (UINT)DPI_SCALE_COUNT);
    ok(DpiScaleStandard[0] == 100 && DpiScaleStandard[DPI_SCALE_COUNT - 1] == 500,
       "standard list runs %u..%u\n", DpiScaleStandard[0], DpiScaleStandard[DPI_SCALE_COUNT - 1]);
    for (i = 0; i < _countof(Cases); i++)
    {
        largest = 0;
        previous = 1;
        for (j = 0; j < DPI_SCALE_COUNT; j++)
        {
            int offered = DpiScaleIsOffered(Cases[i].Width, Cases[i].Height, DpiScaleStandard[j]);

            ok(!offered || previous, "%lux%lu: %u%% offered after a gap\n",
               Cases[i].Width, Cases[i].Height, DpiScaleStandard[j]);
            if (offered)
                largest = DpiScaleStandard[j];
            previous = offered;
        }
        ok(largest == Cases[i].LargestOffered, "%lux%lu: largest offered %u%%, expected %u%%\n",
           Cases[i].Width, Cases[i].Height, largest, Cases[i].LargestOffered);
        ok(DpiScaleRecommended(Cases[i].Width, Cases[i].Height, TRUE) == Cases[i].RecommendedPortable,
           "%lux%lu portable: recommended %u%%, expected %u%%\n", Cases[i].Width, Cases[i].Height,
           DpiScaleRecommended(Cases[i].Width, Cases[i].Height, TRUE), Cases[i].RecommendedPortable);
        ok(DpiScaleRecommended(Cases[i].Width, Cases[i].Height, FALSE) == 100,
           "%lux%lu desktop: recommended %u%%, expected 100%%\n", Cases[i].Width, Cases[i].Height,
           DpiScaleRecommended(Cases[i].Width, Cases[i].Height, FALSE));
    }
    ok(DpiScaleIsOffered(800, 600, 100), "100%% must always be offered\n");
    ok(!DpiScaleIsOffered(800, 600, 125), "800x600 must not offer 125%%\n");
    ok(DpiScaleIsPortableChassis(0x09) && DpiScaleIsPortableChassis(0x1F | 0x80), "laptop/convertible\n");
    ok(!DpiScaleIsPortableChassis(0x03) && !DpiScaleIsPortableChassis(0x23), "desktop/mini PC\n");
}
