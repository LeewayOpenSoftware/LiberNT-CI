/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Round-trip test for the SPI_GET/SETCAPTIONALIGNMENT system parameter
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"

static
int
GetAlign(void)
{
    int value = -1;
    BOOL ret = SystemParametersInfoW(SPI_GETCAPTIONALIGNMENT, 0, &value, 0);
    ok(ret, "SPI_GETCAPTIONALIGNMENT failed, error %lu\n", GetLastError());
    return value;
}

START_TEST(SpiCaptionAlignment)
{
    int original, value;
    BOOL ret;

    /* Record the current value so the machine is left as we found it. */
    original = GetAlign();
    ok(original == CAPTIONALIGN_LEFT || original == CAPTIONALIGN_CENTER,
       "unexpected initial alignment %d\n", original);

    /* Set centre and read it back. */
    ret = SystemParametersInfoW(SPI_SETCAPTIONALIGNMENT, CAPTIONALIGN_CENTER, NULL, 0);
    ok(ret, "SPI_SETCAPTIONALIGNMENT(centre) failed, error %lu\n", GetLastError());
    value = GetAlign();
    ok(value == CAPTIONALIGN_CENTER, "expected centre (1), got %d\n", value);

    /* Set left and read it back. */
    ret = SystemParametersInfoW(SPI_SETCAPTIONALIGNMENT, CAPTIONALIGN_LEFT, NULL, 0);
    ok(ret, "SPI_SETCAPTIONALIGNMENT(left) failed, error %lu\n", GetLastError());
    value = GetAlign();
    ok(value == CAPTIONALIGN_LEFT, "expected left (0), got %d\n", value);

    /* Any non-zero uiParam must normalise to centre, never a raw out-of-range value. */
    ret = SystemParametersInfoW(SPI_SETCAPTIONALIGNMENT, 5, NULL, 0);
    ok(ret, "SPI_SETCAPTIONALIGNMENT(5) failed, error %lu\n", GetLastError());
    value = GetAlign();
    ok(value == CAPTIONALIGN_CENTER, "out-of-range uiParam should normalise to centre, got %d\n", value);

    /* Restore the original value. */
    ret = SystemParametersInfoW(SPI_SETCAPTIONALIGNMENT, original, NULL, 0);
    ok(ret, "restore SPI_SETCAPTIONALIGNMENT failed, error %lu\n", GetLastError());
    value = GetAlign();
    ok(value == original, "failed to restore alignment, got %d expected %d\n", value, original);
}
