/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         The qsort_s and bsearch_s CRT-style exports
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef int (NTAPI *PRTL_CONTEXT_COMPARE)(PVOID Context, PVOID Element1, PVOID Element2);

#define RTL_EINVAL 22

static VOID
RtlpQsortSwap(_Inout_ PUCHAR Left,
              _Inout_ PUCHAR Right,
              _In_ SIZE_T Width)
{
    while (Width--)
    {
        UCHAR Temp = *Left;
        *Left++ = *Right;
        *Right++ = Temp;
    }
}

static PUCHAR
RtlpQsortPartition(_In_ PUCHAR Base,
                   _In_ PUCHAR Pivot,
                   _In_ PUCHAR End,
                   _In_ SIZE_T Width,
                   _In_ PRTL_CONTEXT_COMPARE Compare,
                   _In_ PVOID Context)
{
    PUCHAR Store = Base;

    RtlpQsortSwap(Pivot, End - Width, Width);
    Pivot = End - Width;

    for (; Base < End - Width; Base += Width)
    {
        if (Compare(Context, Base, Pivot) < 0)
        {
            RtlpQsortSwap(Base, Store, Width);
            Store += Width;
        }
    }

    RtlpQsortSwap(Store, Pivot, Width);
    return Store;
}

static VOID
RtlpQsortBody(_In_ PUCHAR Base,
              _In_ PUCHAR End,
              _In_ SIZE_T Width,
              _In_ PRTL_CONTEXT_COMPARE Compare,
              _In_ PVOID Context)
{
    while (End - Base > Width)
    {
        PUCHAR Middle = Base + (((End - Base) / Width) / 2) * Width;
        PUCHAR Pivot, NewPivot;

        if (End - Base >= 3 * Width)
        {
            PUCHAR Last = End - Width;
            if (Compare(Context, Base, Middle) > 0)
                RtlpQsortSwap(Base, Middle, Width);
            if (Compare(Context, Middle, Last) > 0)
                RtlpQsortSwap(Middle, Last, Width);
            if (Compare(Context, Base, Middle) > 0)
                RtlpQsortSwap(Base, Middle, Width);
        }

        Pivot = Middle;
        NewPivot = RtlpQsortPartition(Base, Pivot, End, Width, Compare, Context);

        if (NewPivot - Base < End - (NewPivot + Width))
        {
            RtlpQsortBody(Base, NewPivot, Width, Compare, Context);
            Base = NewPivot + Width;
        }
        else
        {
            RtlpQsortBody(NewPivot + Width, End, Width, Compare, Context);
            End = NewPivot;
        }
    }
}

VOID
NTAPI
qsort_s(IN PVOID Base,
        IN SIZE_T NumberOfElements,
        IN SIZE_T ElementSize,
        IN PRTL_CONTEXT_COMPARE CompareFunction,
        IN PVOID Context)
{
    if (CompareFunction == NULL || ElementSize == 0)
        return;
    if (Base == NULL && NumberOfElements != 0)
        return;

    if (NumberOfElements > 1)
        RtlpQsortBody(Base, (PUCHAR)Base + NumberOfElements * ElementSize, ElementSize,
                      CompareFunction, Context);
}

INT
NTAPI
bsearch_s(IN PVOID Key,
          IN PVOID Base,
          IN SIZE_T NumberOfElements,
          IN SIZE_T ElementSize,
          IN PRTL_CONTEXT_COMPARE CompareFunction,
          IN PVOID Context,
          OUT PVOID *Result)
{
    PUCHAR Low, High;

    if (Result != NULL)
        *Result = NULL;
    if ((Key == NULL) || (CompareFunction == NULL) || (Result == NULL) ||
        (ElementSize == 0) || ((Base == NULL) && (NumberOfElements != 0)))
        return RTL_EINVAL;

    Low = (PUCHAR)Base;
    High = (PUCHAR)Base + NumberOfElements * ElementSize;
    while (Low < High)
    {
        PUCHAR Middle = Low + ((((High - Low) / ElementSize) / 2)) * ElementSize;
        int Compare = CompareFunction(Context, Key, Middle);

        if (Compare == 0)
        {
            *Result = Middle;
            return 0;
        }
        if (Compare < 0)
            High = Middle;
        else
            Low = Middle + ElementSize;
    }

    return 0;
}
