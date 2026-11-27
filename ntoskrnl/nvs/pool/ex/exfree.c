/*
 * PROJECT:     LiberNT Pool Allocator
 * FILE:        ntoskrnl/nvs/pool/ex/exfree.c
 * PURPOSE:     Executive pool deallocation interfaces
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "expool.h"

#define NDEBUG
#if !defined(POOL_HOST_TEST)
#include <debug.h>
#endif

#if !defined(POOL_HOST_TEST)
static
BOOLEAN
ExpPoolTagCheckEnabled(VOID)
{
    return (BOOLEAN)(VfPoolTrackingEnabled || (ExpPoolFlags & EX_POOL_FLAG_CHECK_TAGS) != 0);
}

static
VOID
ExpPoolLogTagMismatchOnce(
    _In_ PVOID P,
    _In_ ULONG TagToFree,
    _In_ ULONG ActualTag)
{
    static LONG ExpPoolTagMismatchLogged;

    if (InterlockedCompareExchange(&ExpPoolTagMismatchLogged, 1, 0) == 0)
    {
        DbgPrint("ExFreePoolWithTag: pool tag mismatch at %p, freed as 0x%08lx, allocated as 0x%08lx\n",
                 P, TagToFree, ActualTag);
    }
}

static
VOID
ExpPoolCheckTag(
    _In_ PVOID P,
    _In_ ULONG TagToFree)
{
    ULONG ActualTag = 0;
    BOOLEAN HaveTag = FALSE;

    if ((ExpPoolFlags & EX_POOL_FLAG_SPECIAL_POOL) && MmIsSpecialPoolAddress(P))
    {
        HaveTag = MmQuerySpecialPoolTag(P, &ActualTag);
    }
    else
    {
        PPOOL_HEAP CheckHeap;
        ULONG CheckHeapIndex;
        POOL_BLOCK_INFO CheckInfo;

        CheckHeap = ExpPoolHeapFromAddress(P, &CheckHeapIndex);
        if (CheckHeap != NULL && PoolHeapQuery(CheckHeap, P, &CheckInfo))
        {
            ActualTag = CheckInfo.Tag;
            HaveTag = TRUE;
        }
    }

    if (HaveTag &&
        (TagToFree & ~(ULONG)PROTECTED_POOL) != (ActualTag & ~(ULONG)PROTECTED_POOL))
    {
        ExpPoolLogTagMismatchOnce(P, TagToFree, ActualTag);
    }
}
#endif

VOID
NTAPI
ExFreePoolWithTag(
    IN PVOID P,
    IN ULONG TagToFree)
{
    POOL_BLOCK_INFO Info;
    PPOOL_HEAP Heap;
    ULONG HeapIndex;

#if defined(POOL_HOST_TEST)
    UNREFERENCED_PARAMETER(TagToFree);
#endif

    if (P == NULL)
        return;

#if !defined(POOL_HOST_TEST)
    if (TagToFree != 0 && ExpPoolTagCheckEnabled())
        ExpPoolCheckTag(P, TagToFree);
#endif

#if !defined(POOL_HOST_TEST)
    if (VfPoolTrackingEnabled)
        VfUntrackPool(P);
#endif

    if ((ExpPoolFlags & EX_POOL_FLAG_SPECIAL_POOL) && MmIsSpecialPoolAddress(P))
    {
        MmFreeSpecialPool(P);
        return;
    }

    Heap = ExpPoolHeapFromAddress(P, &HeapIndex);
    if (Heap == NULL)
        KeBugCheckEx(BAD_POOL_CALLER, 0x46, (ULONG_PTR)P, 0, 0);

    if (HeapIndex == EX_POOL_HEAP_PAGED && KeGetCurrentIrql() > APC_LEVEL)
        KeBugCheckEx(BAD_POOL_CALLER, 0x09, KeGetCurrentIrql(), PagedPool, (ULONG_PTR)P);

    if (!PoolHeapFree(Heap, P, &Info))
        KeBugCheckEx(BAD_POOL_CALLER, 0x07, (ULONG_PTR)P, HeapIndex, 0);

    PoolTagRelease(&ExpPoolState.Tracker, Info.Tag, ExpPoolClass(HeapIndex), Info.Size);

    if (Info.Flags & EX_POOL_BLOCK_QUOTA)
    {
        PEPROCESS Process = ExpPoolQuotaProcess(P, Info.Trailer);

        POOL_ASSERT(Info.Size >= sizeof(PEPROCESS) && Process != NULL);
        if (Process != NULL)
        {
            PsReturnPoolQuota(Process, ExpPoolClass(HeapIndex), Info.Size);
            ObDereferenceObject(Process);
        }
    }
}

VOID
NTAPI
ExFreePool(
    PVOID P)
{
    ExFreePoolWithTag(P, 0);
}

VOID
NTAPI
ExFreePool2(
    IN PVOID P,
    IN ULONG Tag,
    IN PCPOOL_EXTENDED_PARAMETER ExtendedParameters,
    IN ULONG ExtendedParametersCount)
{
    UNREFERENCED_PARAMETER(ExtendedParameters);
    UNREFERENCED_PARAMETER(ExtendedParametersCount);
    ExFreePoolWithTag(P, Tag);
}
