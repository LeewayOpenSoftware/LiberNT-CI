/*
 * PROJECT:     LiberNT Pool Allocator
 * FILE:        ntoskrnl/nvs/pool/ex/exquota.c
 * PURPOSE:     Executive pool quota accounting
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "expool.h"

#define NDEBUG
#if !defined(POOL_HOST_TEST)
#include <debug.h>
#endif

FORCEINLINE
PEPROCESS *
ExpPoolQuotaSlot(
    _In_ PVOID Block,
    _In_ SIZE_T BlockSize)
{
    return (PEPROCESS *)((ULONG_PTR)Block + BlockSize - sizeof(PEPROCESS));
}

PEPROCESS
ExpPoolQuotaProcess(
    _In_ PVOID Block,
    _In_ ULONG_PTR Trailer)
{
    ULONG_PTR Value = Trailer ^ ExpPoolQuotaCookie ^ (ULONG_PTR)Block;
    PEPROCESS Process = (PEPROCESS)Value;

    if (Value == 0 ||
        (Value & (MEMORY_ALLOCATION_ALIGNMENT - 1)) != 0 ||
        Value < (ULONG_PTR)MmSystemRangeStart ||
        !MmIsAddressValid(Process) ||
        (Process->Pcb.Header.Type & KOBJECT_TYPE_MASK) != ProcessObject)
    {
        KeBugCheckEx(BAD_POOL_HEADER, 0x24, (ULONG_PTR)Block, Trailer, 0);
    }

    return Process;
}

PVOID
NTAPI
ExAllocatePoolWithQuotaTag(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag)
{
    PEPROCESS Process = PsGetCurrentProcess();
    ULONG HeapIndex = (ULONG)PoolType & EX_POOL_BASE_TYPE_MASK;
    BOOLEAN Raise = TRUE;
    POOL_BLOCK_INFO Info;
    PPOOL_HEAP Heap;
    NTSTATUS Status;
    PVOID Block;
    BOOLEAN Updated;

    if (PoolType & POOL_QUOTA_FAIL_INSTEAD_OF_RAISE)
    {
        Raise = FALSE;
        PoolType &= ~POOL_QUOTA_FAIL_INSTEAD_OF_RAISE;
    }

    if (Process == PsInitialSystemProcess || NumberOfBytes > (SIZE_T)-1 - sizeof(PEPROCESS))
    {
        Block = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
        if (Block == NULL && Raise)
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);

        return Block;
    }

    Block = ExAllocatePoolWithTag(PoolType, NumberOfBytes + sizeof(PEPROCESS), Tag);
    if (Block == NULL)
    {
        if (Raise)
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);

        return NULL;
    }

    Heap = ExpPoolHeapFromAddress(Block, &HeapIndex);
    if (Heap == NULL || !PoolHeapQuery(Heap, Block, &Info))
        return Block;

    POOL_ASSERT(Info.Size >= NumberOfBytes + sizeof(PEPROCESS));
    POOL_ASSERT((Info.Flags & EX_POOL_BLOCK_QUOTA) == 0);
    POOL_ASSERT(Process != NULL);
    Status = PsChargeProcessPoolQuota(Process, ExpPoolClass(HeapIndex), Info.Size);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Block, Tag);
        if (Raise)
            ExRaiseStatus(Status);

        return NULL;
    }

    ObReferenceObject(Process);
    *ExpPoolQuotaSlot(Block, Info.Size) =
        (PEPROCESS)((ULONG_PTR)Process ^ ExpPoolQuotaCookie ^ (ULONG_PTR)Block);
    Updated = PoolHeapSetFlags(Heap, Block, Info.Flags | EX_POOL_BLOCK_QUOTA);
    POOL_ASSERT(Updated);
    UNREFERENCED_PARAMETER(Updated);
    return Block;
}

PVOID
NTAPI
ExAllocatePoolWithQuota(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes)
{
    return ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, TAG_NONE);
}

VOID
NTAPI
ExReturnPoolQuota(
    IN PVOID P)
{
    POOL_BLOCK_INFO Info;
    PPOOL_HEAP Heap;
    ULONG HeapIndex;
    PEPROCESS Process;
    BOOLEAN Updated;

    if ((ExpPoolFlags & EX_POOL_FLAG_SPECIAL_POOL) && MmIsSpecialPoolAddress(P))
        return;

    Heap = ExpPoolHeapFromAddress(P, &HeapIndex);
    if (Heap == NULL || !PoolHeapQuery(Heap, P, &Info) || !(Info.Flags & EX_POOL_BLOCK_QUOTA))
        return;

    POOL_ASSERT(Info.Size >= sizeof(PEPROCESS));
    POOL_ASSERT(Info.Trailer == (ULONG_PTR)*ExpPoolQuotaSlot(P, Info.Size));
    Process = ExpPoolQuotaProcess(P, Info.Trailer);
    *ExpPoolQuotaSlot(P, Info.Size) = NULL;
    Updated = PoolHeapSetFlags(Heap, P, Info.Flags & ~EX_POOL_BLOCK_QUOTA);
    POOL_ASSERT(Updated);
    UNREFERENCED_PARAMETER(Updated);

    if (Process != NULL)
    {
        PsReturnPoolQuota(Process, ExpPoolClass(HeapIndex), Info.Size);
        ObDereferenceObject(Process);
    }
}
