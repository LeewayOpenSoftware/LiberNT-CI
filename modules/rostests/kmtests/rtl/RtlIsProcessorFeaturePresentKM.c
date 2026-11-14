/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the kernel export of RtlIsProcessorFeaturePresent
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

NTSYSAPI
BOOLEAN
NTAPI
RtlIsProcessorFeaturePresent(
    _In_ ULONG ProcessorFeature);

START_TEST(RtlIsProcessorFeaturePresentKM)
{
    ULONG Feature;
    BOOLEAN FromRtl, FromEx;

    /* The kernel keeps one table of processor features; both exports read it */
    for (Feature = 0; Feature < PROCESSOR_FEATURE_MAX; Feature++)
    {
        FromRtl = RtlIsProcessorFeaturePresent(Feature);
        FromEx = ExIsProcessorFeaturePresent(Feature);
        ok(!FromRtl == !FromEx, "Feature %lu: Rtl says %u, Ex says %u\n", Feature, FromRtl, FromEx);
    }

    /* A feature number past the table is never present */
    ok_eq_bool(RtlIsProcessorFeaturePresent(PROCESSOR_FEATURE_MAX), FALSE);
    ok_eq_bool(RtlIsProcessorFeaturePresent(0xFFFFFFFF), FALSE);

#if defined(_M_AMD64)
    /* Every amd64 processor has these */
    ok_eq_bool(RtlIsProcessorFeaturePresent(PF_COMPARE_EXCHANGE_DOUBLE), TRUE);
    ok_eq_bool(RtlIsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE), TRUE);
#endif
}
