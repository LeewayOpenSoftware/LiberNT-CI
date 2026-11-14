/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: pool allocations of a verified driver go to special pool
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define VF_POOL2_TYPE_FLAGS (POOL_FLAG_NON_PAGED | POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_PAGED)

static
VOID
VfpCount(
    _Inout_ PULONG Counter)
{
    InterlockedIncrement((PLONG)Counter);
}

static
PVOID
VfpAllocateSpecial(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ ULONG SpecialType,
    _In_ BOOLEAN Zero)
{
    PVOID Block;

    VfpCount(&MmVerifierData.AllocationsAttempted);
    if (Tag == 0 || Tag == TAG_NONE)
        VfpCount(&MmVerifierData.AllocationsWithNoTag);

    Block = MmAllocateSpecialPool(NumberOfBytes, Tag, PoolType, SpecialType);
    if (Block == NULL)
        return NULL;

    if (Zero)
        RtlZeroMemory(Block, NumberOfBytes);

    VfpCount(&MmVerifierData.AllocationsSucceeded);
    VfpCount(&MmVerifierData.AllocationsSucceededSpecialPool);
    return Block;
}

static
PVOID
VfpCountOrdinary(
    _In_opt_ PVOID Block)
{
    VfpCount(Block != NULL ? &MmVerifierData.AllocationsSucceeded : &MmVerifierData.AllocationsFailed);
    return Block;
}

static
ULONG
VfpSpecialType(
    _In_ EX_POOL_PRIORITY Priority)
{
    ULONG Request = (ULONG)Priority & 0xF;

    if (Request == LowPoolPrioritySpecialPoolOverrun)
        return 0;

    if (Request == LowPoolPrioritySpecialPoolUnderrun)
        return 1;

    return 2;
}

static
PVOID
NTAPI
VfExAllocatePoolWithTagPriority(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ EX_POOL_PRIORITY Priority)
{
    PVOID Block = VfpAllocateSpecial(PoolType, NumberOfBytes, Tag, VfpSpecialType(Priority),
                                     (BOOLEAN)((PoolType & POOL_ZERO_ALLOCATION) != 0));

    if (Block != NULL)
        return Block;

    return VfpCountOrdinary(ExAllocatePoolWithTagPriority(PoolType, NumberOfBytes, Tag, Priority));
}

static
PVOID
NTAPI
VfExAllocatePoolWithTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    PVOID Block = VfpAllocateSpecial(PoolType, NumberOfBytes, Tag, 2,
                                     (BOOLEAN)((PoolType & POOL_ZERO_ALLOCATION) != 0));

    if (Block != NULL)
        return Block;

    return VfpCountOrdinary(ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag));
}

static
PVOID
NTAPI
VfExAllocatePool(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes)
{
    return VfExAllocatePoolWithTag(PoolType, NumberOfBytes, TAG_NONE);
}

static
PVOID
NTAPI
VfExAllocatePoolWithQuotaTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    PVOID Block = VfpAllocateSpecial((POOL_TYPE)(PoolType & ~POOL_QUOTA_FAIL_INSTEAD_OF_RAISE), NumberOfBytes, Tag, 2,
                                     FALSE);

    if (Block != NULL)
        return Block;

    return VfpCountOrdinary(ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, Tag));
}

static
PVOID
NTAPI
VfExAllocatePoolWithQuota(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes)
{
    return VfExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, TAG_NONE);
}

static
PVOID
VfpAllocatePool2(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    POOL_FLAGS TypeFlags = Flags & VF_POOL2_TYPE_FLAGS;
    POOL_TYPE PoolType;

    if (Tag == 0 || TypeFlags == 0 || (TypeFlags & (TypeFlags - 1)) != 0)
        return NULL;

    PoolType = (TypeFlags & POOL_FLAG_PAGED) ? PagedPool
             : (TypeFlags & POOL_FLAG_NON_PAGED_EXECUTE) ? NonPagedPoolExecute
                                                         : NonPagedPoolNx;
    if (Flags & POOL_FLAG_CACHE_ALIGNED)
        PoolType = (POOL_TYPE)(PoolType | CACHE_ALIGNED_POOL_MASK);

    return VfpAllocateSpecial(PoolType, NumberOfBytes, Tag, 2, (BOOLEAN)((Flags & POOL_FLAG_UNINITIALIZED) == 0));
}

static
PVOID
NTAPI
VfExAllocatePool2(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    PVOID Block = VfpAllocatePool2(Flags, NumberOfBytes, Tag);

    if (Block != NULL)
        return Block;

    return VfpCountOrdinary(ExAllocatePool2(Flags, NumberOfBytes, Tag));
}

static
PVOID
NTAPI
VfExAllocatePool3(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_reads_opt_(ExtendedParametersCount) PCPOOL_EXTENDED_PARAMETER ExtendedParameters,
    _In_ ULONG ExtendedParametersCount)
{
    PVOID Block = VfpAllocatePool2(Flags, NumberOfBytes, Tag);

    if (Block != NULL)
        return Block;

    return VfpCountOrdinary(ExAllocatePool3(Flags, NumberOfBytes, Tag, ExtendedParameters, ExtendedParametersCount));
}

const VF_THUNK VfPoolThunks[] =
{
    { (ULONG_PTR)ExAllocatePool, (ULONG_PTR)VfExAllocatePool },
    { (ULONG_PTR)ExAllocatePoolWithTag, (ULONG_PTR)VfExAllocatePoolWithTag },
    { (ULONG_PTR)ExAllocatePoolWithQuota, (ULONG_PTR)VfExAllocatePoolWithQuota },
    { (ULONG_PTR)ExAllocatePoolWithQuotaTag, (ULONG_PTR)VfExAllocatePoolWithQuotaTag },
    { (ULONG_PTR)ExAllocatePoolWithTagPriority, (ULONG_PTR)VfExAllocatePoolWithTagPriority },
    { (ULONG_PTR)ExAllocatePool2, (ULONG_PTR)VfExAllocatePool2 },
    { (ULONG_PTR)ExAllocatePool3, (ULONG_PTR)VfExAllocatePool3 },
};

const ULONG VfPoolThunkCount = RTL_NUMBER_OF(VfPoolThunks);
