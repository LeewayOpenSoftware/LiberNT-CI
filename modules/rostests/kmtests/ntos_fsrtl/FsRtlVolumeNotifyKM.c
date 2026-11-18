/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for FsRtlIncrementCcFastMdlReadWait
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

START_TEST(FsRtlVolumeNotifyKM)
{
    ULONG Before, After;

    Before = CcFastMdlReadWait;
    FsRtlIncrementCcFastMdlReadWait();
    After = CcFastMdlReadWait;
    ok_eq_ulong(After, Before + 1);
}
