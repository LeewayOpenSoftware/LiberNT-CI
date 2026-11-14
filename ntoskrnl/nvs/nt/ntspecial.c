/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/nt/ntspecial.c
 * PURPOSE:     Special pool on the guarded system region: selection, bug checks
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <nvs/nt/mint.h>
#include <nvs/pool/ex/expool.h>
#include <nvs/pool/include/poolspecial.h>

#define MI_SPECIAL_POOL_MINIMUM_PAGES   64
#define MI_SPECIAL_POOL_MAXIMUM_PAGES   32768
#define MI_SPECIAL_POOL_MEMORY_SHARE    16
#define MI_SPECIAL_POOL_SLOT_RATIO      4
#define MI_SPECIAL_POOL_RESERVE_PAGES   16
#define MI_SPECIAL_POOL_CACHE_ALIGNMENT 64

#define MI_SPECIAL_POOL_NOT_ALLOCATED   0x20
#define MI_SPECIAL_POOL_CORRUPT_BEFORE  0x23
#define MI_SPECIAL_POOL_CORRUPT_AFTER   0x24
#define MI_SPECIAL_POOL_ALLOCATE_IRQL   0x30
#define MI_SPECIAL_POOL_FREE_IRQL       0x31

ULONG MmSpecialPoolTag;
ULONG MmSpecialPoolCatchOverruns = 1;

static PMI_GUARD_REGION MiSpecialPoolRegion;
static BOOLEAN MiSpecialPoolUnderruns;

VOID
NTAPI
MiInitializeSpecialPool(VOID)
{
    PMI_GUARD_REGION Region;
    ULONG64 Budget;
    NTSTATUS Status;
    ULONG Slots;

    if (MmSpecialPoolTag == 0 && MmVerifyDriverBuffer[0] == UNICODE_NULL)
        return;

    Budget = (ULONG64)MmNumberOfPhysicalPages / MI_SPECIAL_POOL_MEMORY_SHARE;
    if (Budget < MI_SPECIAL_POOL_MINIMUM_PAGES)
        Budget = MI_SPECIAL_POOL_MINIMUM_PAGES;
    if (Budget > MI_SPECIAL_POOL_MAXIMUM_PAGES)
        Budget = MI_SPECIAL_POOL_MAXIMUM_PAGES;

    Slots = (ULONG)Budget * MI_SPECIAL_POOL_SLOT_RATIO;
    Status = MiGuardRegionCreate(&MiSystem, Slots, 1, MI_SPECIAL_POOL_RESERVE_PAGES, Slots / 2, Budget,
                                 sizeof(POOL_SPECIAL_RECORD), &Region);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("Special pool: not enabled, status 0x%08lx\n", Status);
        return;
    }

    MiSpecialPoolUnderruns = (BOOLEAN)(MmSpecialPoolCatchOverruns == 0);
    MiSpecialPoolRegion = Region;
    ExpPoolFlags |= EX_POOL_FLAG_SPECIAL_POOL;

    DbgPrint("Special pool: selector 0x%08lx, %s, %lu slots at %p, at most %I64u pages in use\n",
             MmSpecialPoolTag, MiSpecialPoolUnderruns ? "underruns" : "overruns", Slots,
             (PVOID)(ULONG_PTR)Region->Base, Budget);
}

BOOLEAN
NTAPI
MiIsSpecialPoolEnabled(VOID)
{
    return (BOOLEAN)(MiSpecialPoolRegion != NULL);
}

BOOLEAN
NTAPI
MmUseSpecialPool(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    return (BOOLEAN)(MiSpecialPoolRegion != NULL && PoolSpecialSelect(MmSpecialPoolTag, NumberOfBytes, Tag));
}

BOOLEAN
NTAPI
MmIsSpecialPoolAddress(
    _In_ PVOID P)
{
    return (BOOLEAN)(MiGuardQuery(MiSpecialPoolRegion, (ULONG64)(ULONG_PTR)P, NULL, NULL) != MiGuardOutside);
}

BOOLEAN
NTAPI
MmIsSpecialPoolAddressFree(
    _In_ PVOID P)
{
    return (BOOLEAN)(MiGuardQuery(MiSpecialPoolRegion, (ULONG64)(ULONG_PTR)P, NULL, NULL) == MiGuardFreed);
}

PVOID
NTAPI
MmAllocateSpecialPool(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ POOL_TYPE PoolType,
    _In_ ULONG SpecialType)
{
    BOOLEAN AtStart = (SpecialType == 0) ? FALSE : (SpecialType == 1) ? TRUE : MiSpecialPoolUnderruns;
    BOOLEAN Paged = (BOOLEAN)(((ULONG)PoolType & EX_POOL_BASE_TYPE_MASK) != 0);
    PMI_GUARD_REGION Region = MiSpecialPoolRegion;
    KIRQL Irql = KeGetCurrentIrql();
    PVOID Record;
    ULONG64 Page;
    ULONG Offset;

    if (Region == NULL)
        return NULL;

    if (Irql > DISPATCH_LEVEL || (Paged && Irql > APC_LEVEL))
    {
        KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, Irql, (ULONG_PTR)PoolType, NumberOfBytes,
                     MI_SPECIAL_POOL_ALLOCATE_IRQL);
    }

    if (!PoolSpecialPlace(NumberOfBytes,
                          ((ULONG)PoolType & CACHE_ALIGNED_POOL_MASK) ? MI_SPECIAL_POOL_CACHE_ALIGNMENT : 0,
                          AtStart, &Offset))
    {
        return NULL;
    }

    if (!NT_SUCCESS(MiGuardAllocate(Region,
                                    (ExpPoolHeapFromType(PoolType) == EX_POOL_HEAP_EXECUTABLE)
                                        ? MI_PROT_EXECUTE_READWRITE : MI_PROT_READWRITE,
                                    &Page, &Record)))
    {
        return NULL;
    }

    return PoolSpecialPrepare(Record, (PVOID)(ULONG_PTR)Page, NumberOfBytes, Offset, Tag, (ULONG)PoolType);
}

VOID
NTAPI
MmFreeSpecialPool(
    _In_ PVOID P)
{
    PMI_GUARD_REGION Region = MiSpecialPoolRegion;
    KIRQL Irql = KeGetCurrentIrql();
    PPOOL_SPECIAL_RECORD Record;
    ULONG_PTR Corrupt;
    PVOID Context;
    ULONG64 Page;

    if (MiGuardQuery(Region, (ULONG64)(ULONG_PTR)P, &Page, &Context) != MiGuardLive ||
        !PoolSpecialOwns(Context, (PVOID)(ULONG_PTR)Page, P))
    {
        KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, (ULONG_PTR)P, 0, 0, MI_SPECIAL_POOL_NOT_ALLOCATED);
    }

    Record = Context;
    if (Irql > DISPATCH_LEVEL || ((Record->Flags & EX_POOL_BASE_TYPE_MASK) && Irql > APC_LEVEL))
    {
        KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, Irql, Record->Flags, (ULONG_PTR)P,
                     MI_SPECIAL_POOL_FREE_IRQL);
    }

    switch (PoolSpecialCheck(Record, (PVOID)(ULONG_PTR)Page, &Corrupt))
    {
        case PoolSpecialCorruptBefore:
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, (ULONG_PTR)P, Corrupt, 0,
                         MI_SPECIAL_POOL_CORRUPT_BEFORE);

        case PoolSpecialCorruptAfter:
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, (ULONG_PTR)P, Corrupt, 0,
                         MI_SPECIAL_POOL_CORRUPT_AFTER);

        default:
            break;
    }

    if (!NT_SUCCESS(MiGuardFree(Region, Page)))
        KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION, (ULONG_PTR)P, 0, 0, MI_SPECIAL_POOL_NOT_ALLOCATED);

    MiGuardRefill(Region);
}

SIZE_T
NTAPI
MmQuerySpecialPoolBlockSize(
    _In_ PVOID P)
{
    PPOOL_SPECIAL_RECORD Record;
    PVOID Context;
    ULONG64 Page;

    if (MiGuardQuery(MiSpecialPoolRegion, (ULONG64)(ULONG_PTR)P, &Page, &Context) != MiGuardLive ||
        !PoolSpecialOwns(Context, (PVOID)(ULONG_PTR)Page, P))
    {
        return 0;
    }

    Record = Context;
    return Record->Size;
}

VOID
NTAPI
MiCheckSpecialPoolFault(
    _In_ PVOID Address,
    _In_ BOOLEAN Write,
    _In_opt_ PVOID TrapInformation)
{
    PKTRAP_FRAME TrapFrame = TrapInformation;
    ULONG64 Physical;

    if (!MmIsSpecialPoolAddress(Address))
        return;

    if (MiPtTranslate(&MiSystem.SystemSpace, (ULONG64)(ULONG_PTR)Address, &Physical, NULL))
        return;

    KeBugCheckWithTf(PAGE_FAULT_IN_NONPAGED_AREA, (ULONG_PTR)Address, Write,
                     (TrapFrame != NULL) ? (ULONG_PTR)KeGetTrapFramePc(TrapFrame) : 0, 0, TrapFrame);
}
