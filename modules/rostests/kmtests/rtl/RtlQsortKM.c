/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     qsort_s/bsearch_s round-trip and validation tests
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

static int NTAPI
CompareLongs(_In_ PVOID Context, _In_ PCVOID Left, _In_ PCVOID Right)
{
    LONG L = *(const PLONG)Left;
    LONG R = *(const PLONG)Right;
    LONG Bias = (LONG)(ULONG_PTR)Context;

    return (L + Bias == R + Bias) ? 0 : (L + Bias < R + Bias) ? -1 : 1;
}

START_TEST(RtlQsortKM)
{
    static const ULONG Values[] = { 42, 7, 1900, 3, 65535, 0, 99, 12 };
    ULONG Sorted[ARRAYSIZE(Values)];
    PVOID Found;
    ULONG i;
    INT Error;

    RtlCopyMemory(Sorted, Values, sizeof(Sorted));

    qsort_s(Sorted, ARRAYSIZE(Sorted), sizeof(ULONG), CompareLongs, (PVOID)(ULONG_PTR)0);
    for (i = 1; i < ARRAYSIZE(Sorted); i++)
        ok(Sorted[i - 1] <= Sorted[i], "array not sorted at %lu (%lu > %lu)\n",
           i, Sorted[i - 1], Sorted[i]);
    ok(Sorted[0] == 0 && Sorted[ARRAYSIZE(Sorted) - 1] == 65535,
       "sorted endpoints wrong: %lu..%lu\n", Sorted[0], Sorted[ARRAYSIZE(Sorted) - 1]);

    Found = NULL;
    Error = bsearch_s(&Sorted[3], Sorted, ARRAYSIZE(Sorted), sizeof(ULONG),
                      CompareLongs, NULL, &Found);
    ok_eq_hex(Error, 0);
    ok(Found != NULL && *(PULONG)Found == Sorted[3], "existing element not found\n");

    {
        ULONG Missing = 12345;
        Found = (PVOID)(ULONG_PTR)1;
        Error = bsearch_s(&Missing, Sorted, ARRAYSIZE(Sorted), sizeof(ULONG),
                          CompareLongs, NULL, &Found);
        ok_eq_hex(Error, 0);
        ok(Found == NULL, "missing element reported found\n");
    }

    qsort_s(NULL, 0, sizeof(ULONG), CompareLongs, NULL);
    qsort_s(NULL, 5, sizeof(ULONG), CompareLongs, NULL);
    qsort_s(Sorted, ARRAYSIZE(Sorted), 0, CompareLongs, NULL);
    qsort_s(Sorted, ARRAYSIZE(Sorted), sizeof(ULONG), NULL, NULL);

    Found = (PVOID)(ULONG_PTR)1;
    Error = bsearch_s(NULL, Sorted, ARRAYSIZE(Sorted), sizeof(ULONG), CompareLongs, NULL, &Found);
    ok_eq_hex(Error, 22);
    Error = bsearch_s(&Sorted[0], NULL, ARRAYSIZE(Sorted), sizeof(ULONG), CompareLongs, NULL, &Found);
    ok_eq_hex(Error, 22);
    Error = bsearch_s(&Sorted[0], Sorted, ARRAYSIZE(Sorted), 0, CompareLongs, NULL, &Found);
    ok_eq_hex(Error, 22);
    Error = bsearch_s(&Sorted[0], Sorted, ARRAYSIZE(Sorted), sizeof(ULONG), NULL, NULL, &Found);
    ok_eq_hex(Error, 22);
    Error = bsearch_s(&Sorted[0], Sorted, ARRAYSIZE(Sorted), sizeof(ULONG), CompareLongs, NULL, NULL);
    ok_eq_hex(Error, 22);
}
