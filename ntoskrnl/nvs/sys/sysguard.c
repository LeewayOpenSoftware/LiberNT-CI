/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/sys/sysguard.c
 * PURPOSE:     Guarded system region for special pool allocations
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <nvs/include/misys.h>

#define MI_GUARD_FREE  0
#define MI_GUARD_READY 1
#define MI_GUARD_LIVE  2
#define MI_GUARD_BUSY  3

static
ULONG64
MiGuardDataVa(
    _In_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot)
{
    return Region->Base + ((ULONG64)Slot * Region->SlotPages + 1) * PAGE_SIZE;
}

static
VOID
MiGuardSetState(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot,
    _In_ LONG State)
{
    MI_ATOMIC_WRITE32(&Region->State[Slot], State);
}

static
BOOLEAN
MiGuardTakeQueuedLocked(
    _Inout_ PMI_GUARD_REGION Region,
    _Out_ PULONG Slot)
{
    if (Region->QueueCount <= Region->ReuseDelay ||
        Region->CommittedPages + Region->DataPages > Region->BudgetPages)
    {
        return FALSE;
    }

    *Slot = Region->Queue[Region->QueueHead];
    Region->QueueHead = (Region->QueueHead + 1) % Region->SlotCount;
    Region->QueueCount--;
    Region->CommittedPages += Region->DataPages;
    if (Region->CommittedPages > Region->PeakCommittedPages)
        Region->PeakCommittedPages = Region->CommittedPages;
    MiGuardSetState(Region, *Slot, MI_GUARD_BUSY);
    return TRUE;
}

static
VOID
MiGuardReturnFrontLocked(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot)
{
    Region->QueueHead = (Region->QueueHead + Region->SlotCount - 1) % Region->SlotCount;
    Region->Queue[Region->QueueHead] = Slot;
    Region->QueueCount++;
    Region->CommittedPages -= Region->DataPages;
    MiGuardSetState(Region, Slot, MI_GUARD_FREE);
}

static
VOID
MiGuardAppendLocked(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot)
{
    MI_ASSERT(Region->QueueCount < Region->SlotCount);
    Region->Queue[(Region->QueueHead + Region->QueueCount) % Region->SlotCount] = Slot;
    Region->QueueCount++;
    MiGuardSetState(Region, Slot, MI_GUARD_FREE);
}

static
BOOLEAN
MiGuardCommitSlot(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot,
    _In_ ULONG Protection)
{
    return NT_SUCCESS(MiSystemCommitPinned(Region->System, MiGuardDataVa(Region, Slot), Region->DataPages,
                                           Protection));
}

static
VOID
MiGuardProtectSlot(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Slot,
    _In_ ULONG Protection)
{
    NTSTATUS Status = MiSystemProtect(Region->System, MiGuardDataVa(Region, Slot), Region->DataPages, Protection);

    MI_ASSERT(NT_SUCCESS(Status));
    UNREFERENCED_PARAMETER(Status);
}

NTSTATUS
MiGuardRegionCreate(
    _Inout_ PMI_SYSTEM System,
    _In_ ULONG SlotCount,
    _In_ ULONG DataPages,
    _In_ ULONG ReservePages,
    _In_ ULONG ReuseDelay,
    _In_ ULONG64 BudgetPages,
    _In_ ULONG ContextBytes,
    _Out_ PMI_GUARD_REGION *RegionOut)
{
    PMI_GUARD_REGION Region;
    ULONG64 TotalPages;
    ULONG ReserveTarget;
    NTSTATUS Status;
    ULONG i;

    *RegionOut = NULL;

    if (SlotCount == 0 || DataPages == 0 || ReuseDelay >= SlotCount ||
        BudgetPages < DataPages)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ReserveTarget = (ULONG)(((ULONG64)ReservePages + DataPages - 1) / DataPages);
    if (ReserveTarget > SlotCount - ReuseDelay)
        return STATUS_INVALID_PARAMETER;

    TotalPages = (ULONG64)SlotCount * ((ULONG64)DataPages + 1) + 1;
    if (TotalPages > 0xFFFFFFFFULL)
        return STATUS_INVALID_PARAMETER;

    Region = MI_ALLOCATE(sizeof(*Region));
    if (Region == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Region, sizeof(*Region));

    Region->System = System;
    Region->SlotCount = SlotCount;
    Region->DataPages = DataPages;
    Region->SlotPages = DataPages + 1;
    Region->ReserveTarget = ReserveTarget;
    Region->ReuseDelay = ReuseDelay;
    Region->ContextBytes = ContextBytes;
    Region->BudgetPages = BudgetPages;
    MI_SPIN_INIT(&Region->Lock);

    Region->State = MI_ALLOCATE((SIZE_T)SlotCount * sizeof(LONG));
    Region->Queue = MI_ALLOCATE((SIZE_T)SlotCount * sizeof(ULONG));
    Region->Ready = MI_ALLOCATE((SIZE_T)(ReserveTarget ? ReserveTarget : 1) * sizeof(ULONG));
    Region->Context = ContextBytes ? MI_ALLOCATE((SIZE_T)SlotCount * ContextBytes) : NULL;
    if (Region->State == NULL || Region->Queue == NULL || Region->Ready == NULL ||
        (ContextBytes != 0 && Region->Context == NULL))
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failed;
    }

    for (i = 0; i < SlotCount; i++)
    {
        Region->State[i] = MI_GUARD_FREE;
        Region->Queue[i] = i;
    }
    Region->QueueCount = SlotCount;
    if (ContextBytes != 0)
        RtlZeroMemory(Region->Context, (SIZE_T)SlotCount * ContextBytes);

    Status = MiSystemRegionCreate(System, TotalPages, TRUE, &Region->Vad, &Region->Base);
    if (!NT_SUCCESS(Status))
        goto Failed;

    Region->End = Region->Base + TotalPages * PAGE_SIZE;
    MiGuardRefill(Region);
    *RegionOut = Region;
    return STATUS_SUCCESS;

Failed:
    if (Region->Context != NULL)
        MI_FREE(Region->Context);
    if (Region->Ready != NULL)
        MI_FREE(Region->Ready);
    if (Region->Queue != NULL)
        MI_FREE(Region->Queue);
    if (Region->State != NULL)
        MI_FREE(Region->State);
    MI_FREE(Region);
    return Status;
}

VOID
MiGuardRegionDelete(
    _Inout_ PMI_GUARD_REGION Region)
{
    ULONG i;

    MI_ASSERT(Region->LiveCount == 0);

    for (i = 0; i < Region->ReadyCount; i++)
    {
        MiGuardProtectSlot(Region, Region->Ready[i], MI_PROT_READWRITE);
        MiSystemDecommitPinned(Region->System, MiGuardDataVa(Region, Region->Ready[i]), Region->DataPages);
    }

    MiSystemRegionDelete(Region->System, Region->Vad, TRUE);

    if (Region->Context != NULL)
        MI_FREE(Region->Context);
    MI_FREE(Region->Ready);
    MI_FREE(Region->Queue);
    MI_FREE(Region->State);
    MI_FREE(Region);
}

ULONG
MiGuardRefill(
    _Inout_ PMI_GUARD_REGION Region)
{
    ULONG Added = 0;
    KIRQL OldIrql;
    ULONG Slot;

    for (;;)
    {
        MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
        if (Region->ReadyCount >= Region->ReserveTarget || !MiGuardTakeQueuedLocked(Region, &Slot))
        {
            MI_SPIN_RELEASE(&Region->Lock, OldIrql);
            return Added;
        }
        MI_SPIN_RELEASE(&Region->Lock, OldIrql);

        if (!MiGuardCommitSlot(Region, Slot, MI_PROT_READWRITE))
        {
            MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
            MiGuardReturnFrontLocked(Region, Slot);
            MI_SPIN_RELEASE(&Region->Lock, OldIrql);
            return Added;
        }
        MiGuardProtectSlot(Region, Slot, MI_PROT_NOACCESS);

        MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
        if (Region->ReadyCount < Region->ReserveTarget)
        {
            Region->Ready[Region->ReadyCount++] = Slot;
            MiGuardSetState(Region, Slot, MI_GUARD_READY);
            MI_SPIN_RELEASE(&Region->Lock, OldIrql);
            Added++;
            continue;
        }
        MI_SPIN_RELEASE(&Region->Lock, OldIrql);

        MiGuardProtectSlot(Region, Slot, MI_PROT_READWRITE);
        MiSystemDecommitPinned(Region->System, MiGuardDataVa(Region, Slot), Region->DataPages);
        MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
        MiGuardReturnFrontLocked(Region, Slot);
        MI_SPIN_RELEASE(&Region->Lock, OldIrql);
        return Added;
    }
}

NTSTATUS
MiGuardAllocate(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG Protection,
    _Out_ PULONG64 DataVa,
    _Out_opt_ PVOID *Context)
{
    BOOLEAN FromReady = FALSE;
    KIRQL OldIrql;
    ULONG Slot;

    *DataVa = 0;
    if (Context != NULL)
        *Context = NULL;

    if (Protection != MI_PROT_READWRITE && Protection != MI_PROT_EXECUTE_READWRITE)
        return STATUS_INVALID_PAGE_PROTECTION;

    MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
    if (!MiGuardTakeQueuedLocked(Region, &Slot))
    {
        if (Region->ReadyCount == 0)
        {
            Region->Failures++;
            MI_SPIN_RELEASE(&Region->Lock, OldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Slot = Region->Ready[--Region->ReadyCount];
        FromReady = TRUE;
    }
    MI_SPIN_RELEASE(&Region->Lock, OldIrql);

    if (!FromReady && !MiGuardCommitSlot(Region, Slot, Protection))
    {
        MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
        MiGuardReturnFrontLocked(Region, Slot);
        if (Region->ReadyCount == 0)
        {
            Region->Failures++;
            MI_SPIN_RELEASE(&Region->Lock, OldIrql);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Slot = Region->Ready[--Region->ReadyCount];
        FromReady = TRUE;
        MI_SPIN_RELEASE(&Region->Lock, OldIrql);
    }

    if (FromReady)
        MiGuardProtectSlot(Region, Slot, Protection);

    if (Region->ContextBytes != 0)
        RtlZeroMemory(Region->Context + (SIZE_T)Slot * Region->ContextBytes, Region->ContextBytes);

    MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
    MiGuardSetState(Region, Slot, MI_GUARD_LIVE);
    Region->LiveCount++;
    Region->Allocations++;
    MI_SPIN_RELEASE(&Region->Lock, OldIrql);

    *DataVa = MiGuardDataVa(Region, Slot);
    if (Context != NULL && Region->ContextBytes != 0)
        *Context = Region->Context + (SIZE_T)Slot * Region->ContextBytes;
    return STATUS_SUCCESS;
}

NTSTATUS
MiGuardFree(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ ULONG64 DataVa)
{
    ULONG64 Offset;
    KIRQL OldIrql;
    ULONG Slot;

    if (DataVa < Region->Base || DataVa >= Region->End)
        return STATUS_INVALID_ADDRESS;

    Offset = (DataVa - Region->Base) >> PAGE_SHIFT;
    if ((DataVa & (PAGE_SIZE - 1)) != 0 || Offset % Region->SlotPages != 1)
        return STATUS_INVALID_ADDRESS;

    Slot = (ULONG)(Offset / Region->SlotPages);
    if (Slot >= Region->SlotCount)
        return STATUS_INVALID_ADDRESS;

    MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
    if (MI_ATOMIC_READ32(&Region->State[Slot]) != MI_GUARD_LIVE)
    {
        MI_SPIN_RELEASE(&Region->Lock, OldIrql);
        return STATUS_INVALID_ADDRESS;
    }
    MiGuardSetState(Region, Slot, MI_GUARD_BUSY);
    Region->LiveCount--;
    MI_SPIN_RELEASE(&Region->Lock, OldIrql);

    MiSystemDecommitPinned(Region->System, DataVa, Region->DataPages);

    MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
    Region->CommittedPages -= Region->DataPages;
    Region->Frees++;
    MiGuardAppendLocked(Region, Slot);
    MI_SPIN_RELEASE(&Region->Lock, OldIrql);
    return STATUS_SUCCESS;
}

MI_GUARD_KIND
MiGuardQuery(
    _In_ PMI_GUARD_REGION Region,
    _In_ ULONG64 VirtualAddress,
    _Out_opt_ PULONG64 DataVa,
    _Out_opt_ PVOID *Context)
{
    ULONG64 Page;
    ULONG Slot;

    if (DataVa != NULL)
        *DataVa = 0;
    if (Context != NULL)
        *Context = NULL;

    if (Region == NULL || VirtualAddress < Region->Base || VirtualAddress >= Region->End)
        return MiGuardOutside;

    Page = (VirtualAddress - Region->Base) >> PAGE_SHIFT;
    Slot = (ULONG)(Page / Region->SlotPages);
    if (Slot >= Region->SlotCount || Page % Region->SlotPages == 0)
        return MiGuardGuardPage;

    if (DataVa != NULL)
        *DataVa = MiGuardDataVa(Region, Slot);
    if (Context != NULL && Region->ContextBytes != 0)
        *Context = Region->Context + (SIZE_T)Slot * Region->ContextBytes;

    return MI_ATOMIC_READ32(&Region->State[Slot]) == MI_GUARD_LIVE ? MiGuardLive : MiGuardFreed;
}

VOID
MiGuardQueryStats(
    _Inout_ PMI_GUARD_REGION Region,
    _Out_ PMI_GUARD_STATS Stats)
{
    KIRQL OldIrql;

    MI_SPIN_ACQUIRE(&Region->Lock, &OldIrql);
    Stats->SlotCount = Region->SlotCount;
    Stats->LiveSlots = Region->LiveCount;
    Stats->ReadySlots = Region->ReadyCount;
    Stats->FreeSlots = Region->QueueCount;
    Stats->CommittedPages = Region->CommittedPages;
    Stats->PeakCommittedPages = Region->PeakCommittedPages;
    Stats->Allocations = Region->Allocations;
    Stats->Frees = Region->Frees;
    Stats->Failures = Region->Failures;
    MI_SPIN_RELEASE(&Region->Lock, OldIrql);
}
