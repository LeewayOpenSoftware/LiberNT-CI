/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Range validation for the SPI_SETFONTSMOOTHINGCONTRAST system parameter
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static
UINT
GetContrast(void)
{
    UINT value = 0;
    BOOL ret = SystemParametersInfoW(SPI_GETFONTSMOOTHINGCONTRAST, 0, &value, 0);
    ok(ret, "SPI_GETFONTSMOOTHINGCONTRAST failed, error %lu\n", GetLastError());
    return value;
}

static
void
ExpectReject(UINT contrast, UINT before)
{
    UINT value;
    BOOL ret;

    SetLastError(0xdeadbeef);
    ret = SystemParametersInfoW(SPI_SETFONTSMOOTHINGCONTRAST, 0, UlongToPtr(contrast), 0);
    ok(!ret, "SPI_SETFONTSMOOTHINGCONTRAST(%u) should have failed\n", contrast);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "expected ERROR_INVALID_PARAMETER for %u, got %lu\n", contrast, GetLastError());
    value = GetContrast();
    ok(value == before, "value changed after rejected %u: got %u expected %u\n", contrast, value, before);
}

static
void
ExpectAccept(UINT contrast)
{
    UINT value;
    BOOL ret = SystemParametersInfoW(SPI_SETFONTSMOOTHINGCONTRAST, 0, UlongToPtr(contrast), 0);
    ok(ret, "SPI_SETFONTSMOOTHINGCONTRAST(%u) failed, error %lu\n", contrast, GetLastError());
    value = GetContrast();
    ok(value == contrast, "expected %u, got %u\n", contrast, value);
}

START_TEST(SpiFontSmoothingContrast)
{
    UINT original;
    BOOL ret;

    /* Record the current value so the machine is left as we found it. */
    original = GetContrast();

    /* Put the value in a known in-range state first. */
    ExpectAccept(1400);

    /* Below the 1000-2200 range: rejected, value unchanged. */
    ExpectReject(999, 1400);

    /* Range boundaries are accepted. */
    ExpectAccept(1000);
    ExpectAccept(2200);

    /* Above the range: rejected, value unchanged (still 2200). */
    ExpectReject(2201, 2200);

    /* Restore the original value (default/out-of-range stored values clamp to 1200). */
    if (original < 1000 || original > 2200)
        original = 1200;
    ret = SystemParametersInfoW(SPI_SETFONTSMOOTHINGCONTRAST, 0, UlongToPtr(original), 0);
    ok(ret, "restore SPI_SETFONTSMOOTHINGCONTRAST failed, error %lu\n", GetLastError());
}
