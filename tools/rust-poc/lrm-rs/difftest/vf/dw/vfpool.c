/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: special pool and pool tracking for the allocations of a verified driver
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define VF_POOL2_TYPE_FLAGS     (POOL_FLAG_NON_PAGED | POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_PAGED)
#define VF_TRACK_BUCKETS        1024
#define VF_TRACK_TAG            'kTfV'
#define VF_LEAKS_PRINTED        16
#define VF_UNLOAD_WITH_POOL     0x62
#define VF_ALLOCATE_ZERO_BYTES  0x00
#define VF_ALLOCATE_PAGED_IRQL  0x01
#define VF_ALLOCATE_IRQL        0x02
#define VF_FREE_PAGED_IRQL      0x11
#define VF_FREE_IRQL            0x12

typedef struct _VF_POOL_ENTRY
{
    LIST_ENTRY Link;
    PVOID Block;
    PVOID Caller;
    SIZE_T Size;
    ULONG Tag;
    BOOLEAN Paged;
} VF_POOL_ENTRY, *PVF_POOL_ENTRY;

BOOLEAN VfPoolTrackingEnabled;

static LIST_ENTRY VfpTrackBucket[VF_TRACK_BUCKETS];
static KSPIN_LOCK VfpTrackLock;

static
VOID
VfpCount(
    _Inout_ PULONG Counter)
{
    InterlockedIncrement((PLONG)Counter);
}

static
PLIST_ENTRY
VfpBucket(
    _In_ PVOID Block)
{
    ULONG_PTR Value = (ULONG_PTR)Block;

    return &VfpTrackBucket[((Value >> PAGE_SHIFT) ^ (Value >> 4)) & (VF_TRACK_BUCKETS - 1)];
}

VOID
NTAPI
VfInitializePoolTracking(VOID)
{
    ULONG i;

    for (i = 0; i < VF_TRACK_BUCKETS; i++)
        InitializeListHead(&VfpTrackBucket[i]);

    KeInitializeSpinLock(&VfpTrackLock);
    VfPoolTrackingEnabled = TRUE;
}

static
VOID
VfpTrack(
    _In_ PVOID Block,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ BOOLEAN Paged,
    _In_ PVOID Caller)
{
    PVF_POOL_ENTRY Entry;
    KIRQL OldIrql;

    if (!VfPoolTrackingEnabled)
        return;

    Entry = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Entry), VF_TRACK_TAG);
    if (Entry == NULL)
    {
        VfpCount(&MmVerifierData.UnTrackedPool);
        return;
    }

    Entry->Block = Block;
    Entry->Caller = Caller;
    Entry->Size = NumberOfBytes;
    Entry->Tag = Tag;
    Entry->Paged = Paged;

    KeAcquireSpinLock(&VfpTrackLock, &OldIrql);
    InsertHeadList(VfpBucket(Block), &Entry->Link);
    if (Paged)
    {
        MmVerifierData.PagedBytes += (ULONG)NumberOfBytes;
        if (++MmVerifierData.CurrentPagedPoolAllocations > MmVerifierData.PeakPagedPoolAllocations)
            MmVerifierData.PeakPagedPoolAllocations = MmVerifierData.CurrentPagedPoolAllocations;
        if (MmVerifierData.PagedBytes > MmVerifierData.PeakPagedBytes)
            MmVerifierData.PeakPagedBytes = MmVerifierData.PagedBytes;
    }
    else
    {
        MmVerifierData.NonPagedBytes += (ULONG)NumberOfBytes;
        if (++MmVerifierData.CurrentNonPagedPoolAllocations > MmVerifierData.PeakNonPagedPoolAllocations)
            MmVerifierData.PeakNonPagedPoolAllocations = MmVerifierData.CurrentNonPagedPoolAllocations;
        if (MmVerifierData.NonPagedBytes > MmVerifierData.PeakNonPagedBytes)
            MmVerifierData.PeakNonPagedBytes = MmVerifierData.NonPagedBytes;
    }
    KeReleaseSpinLock(&VfpTrackLock, OldIrql);
}

VOID
NTAPI
VfUntrackPool(
    _In_ PVOID Block)
{
    PVF_POOL_ENTRY Entry, Found = NULL;
    PLIST_ENTRY Head = VfpBucket(Block);
    PLIST_ENTRY Next;
    KIRQL OldIrql;

    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        return;

    KeAcquireSpinLock(&VfpTrackLock, &OldIrql);
    for (Next = Head->Flink; Next != Head; Next = Next->Flink)
    {
        Entry = CONTAINING_RECORD(Next, VF_POOL_ENTRY, Link);
        if (Entry->Block != Block)
            continue;

        RemoveEntryList(&Entry->Link);
        if (Entry->Paged)
        {
            MmVerifierData.CurrentPagedPoolAllocations--;
            MmVerifierData.PagedBytes -= (ULONG)Entry->Size;
        }
        else
        {
            MmVerifierData.CurrentNonPagedPoolAllocations--;
            MmVerifierData.NonPagedBytes -= (ULONG)Entry->Size;
        }

        Found = Entry;
        break;
    }
    KeReleaseSpinLock(&VfpTrackLock, OldIrql);

    if (Found != NULL)
        ExFreePoolWithTag(Found, VF_TRACK_TAG);
}

VOID
NTAPI
VfCheckPoolAtUnload(
    _In_ PLDR_DATA_TABLE_ENTRY LdrEntry)
{
    ULONG_PTR Start = (ULONG_PTR)LdrEntry->DllBase;
    ULONG_PTR End = Start + LdrEntry->SizeOfImage;
    PVF_POOL_ENTRY Entry;
    PLIST_ENTRY Next;
    ULONG Leaked = 0;
    KIRQL OldIrql;
    ULONG i;

    if (!VfPoolTrackingEnabled || !(LdrEntry->Flags & LDRP_IMAGE_VERIFYING))
        return;

    KeAcquireSpinLock(&VfpTrackLock, &OldIrql);
    for (i = 0; i < VF_TRACK_BUCKETS; i++)
    {
        for (Next = VfpTrackBucket[i].Flink; Next != &VfpTrackBucket[i]; Next = Next->Flink)
        {
            Entry = CONTAINING_RECORD(Next, VF_POOL_ENTRY, Link);
            if ((ULONG_PTR)Entry->Caller < Start || (ULONG_PTR)Entry->Caller >= End)
                continue;

            if (Leaked++ < VF_LEAKS_PRINTED)
            {
                DbgPrint("Driver Verifier: %wZ did not free %p, %Iu bytes, tag %.4s, allocated at %p\n",
                         &LdrEntry->BaseDllName, Entry->Block, Entry->Size, (PCHAR)&Entry->Tag, Entry->Caller);
            }
        }
    }
    KeReleaseSpinLock(&VfpTrackLock, OldIrql);

    if (Leaked != 0)
    {
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_UNLOAD_WITH_POOL,
                     (ULONG_PTR)LdrEntry->BaseDllName.Buffer, 0, Leaked);
    }
}

static
BOOLEAN
VfpIsTrackedPaged(
    _In_ PVOID Block)
{
    PLIST_ENTRY Head = VfpBucket(Block);
    BOOLEAN Paged = FALSE;
    PLIST_ENTRY Next;
    KIRQL OldIrql;

    if (!VfPoolTrackingEnabled)
        return FALSE;

    KeAcquireSpinLock(&VfpTrackLock, &OldIrql);
    for (Next = Head->Flink; Next != Head; Next = Next->Flink)
    {
        PVF_POOL_ENTRY Entry = CONTAINING_RECORD(Next, VF_POOL_ENTRY, Link);

        if (Entry->Block == Block)
        {
            Paged = Entry->Paged;
            break;
        }
    }
    KeReleaseSpinLock(&VfpTrackLock, OldIrql);
    return Paged;
}

static
VOID
VfpCheckAllocate(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes)
{
    KIRQL Irql = KeGetCurrentIrql();
    ULONG Code;

    if (NumberOfBytes == 0)
        Code = VF_ALLOCATE_ZERO_BYTES;
    else if ((PoolType & 1) && Irql > APC_LEVEL)
        Code = VF_ALLOCATE_PAGED_IRQL;
    else if (Irql > DISPATCH_LEVEL)
        Code = VF_ALLOCATE_IRQL;
    else
        return;

    KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, Code, Irql, (ULONG_PTR)PoolType, NumberOfBytes);
}

static
VOID
VfpCheckFree(
    _In_ PVOID Block)
{
    KIRQL Irql = KeGetCurrentIrql();

    if (Irql > DISPATCH_LEVEL)
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_FREE_IRQL, Irql, NonPagedPool, (ULONG_PTR)Block);

    if (Irql > APC_LEVEL && VfpIsTrackedPaged(Block))
        KeBugCheckEx(DRIVER_VERIFIER_DETECTED_VIOLATION, VF_FREE_PAGED_IRQL, Irql, PagedPool, (ULONG_PTR)Block);
}

static
PVOID
VfpFinish(
    _In_opt_ PVOID Block,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ BOOLEAN Paged,
    _In_ PVOID Caller)
{
    if (Block == NULL)
    {
        VfpCount(&MmVerifierData.AllocationsFailed);
        return NULL;
    }

    VfpCount(&MmVerifierData.AllocationsSucceeded);
    VfpTrack(Block, NumberOfBytes, Tag, Paged, Caller);
    return Block;
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

    VfpCheckAllocate(PoolType, NumberOfBytes);
    VfpCount(&MmVerifierData.AllocationsAttempted);
    if (Tag == 0 || Tag == TAG_NONE)
        VfpCount(&MmVerifierData.AllocationsWithNoTag);

    if (!(MmVerifierData.Level & DRIVER_VERIFIER_SPECIAL_POOLING))
        return NULL;

    Block = MmAllocateSpecialPool(NumberOfBytes, Tag, PoolType, SpecialType);
    if (Block == NULL)
        return NULL;

    if (Zero)
        RtlZeroMemory(Block, NumberOfBytes);

    VfpCount(&MmVerifierData.AllocationsSucceededSpecialPool);
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
VfpAllocateWithTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ PVOID Caller)
{
    PVOID Block = VfpAllocateSpecial(PoolType, NumberOfBytes, Tag, 2,
                                     (BOOLEAN)((PoolType & POOL_ZERO_ALLOCATION) != 0));

    if (Block == NULL)
        Block = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);

    return VfpFinish(Block, NumberOfBytes, Tag, (BOOLEAN)((PoolType & 1) != 0), Caller);
}

static
PVOID
VfpAllocateWithQuotaTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ PVOID Caller)
{
    PVOID Block = VfpAllocateSpecial((POOL_TYPE)(PoolType & ~POOL_QUOTA_FAIL_INSTEAD_OF_RAISE), NumberOfBytes, Tag, 2,
                                     FALSE);

    if (Block == NULL)
        Block = ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, Tag);

    return VfpFinish(Block, NumberOfBytes, Tag, (BOOLEAN)((PoolType & 1) != 0), Caller);
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
VfExAllocatePoolWithTagPriority(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ EX_POOL_PRIORITY Priority)
{
    PVOID Caller = _ReturnAddress();
    PVOID Block = VfpAllocateSpecial(PoolType, NumberOfBytes, Tag, VfpSpecialType(Priority),
                                     (BOOLEAN)((PoolType & POOL_ZERO_ALLOCATION) != 0));

    if (Block == NULL)
        Block = ExAllocatePoolWithTagPriority(PoolType, NumberOfBytes, Tag, Priority);

    return VfpFinish(Block, NumberOfBytes, Tag, (BOOLEAN)((PoolType & 1) != 0), Caller);
}

static
PVOID
NTAPI
VfExAllocatePoolWithTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    return VfpAllocateWithTag(PoolType, NumberOfBytes, Tag, _ReturnAddress());
}

static
PVOID
NTAPI
VfExAllocatePool(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes)
{
    return VfpAllocateWithTag(PoolType, NumberOfBytes, TAG_NONE, _ReturnAddress());
}

static
PVOID
NTAPI
VfExAllocatePoolWithQuotaTag(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    return VfpAllocateWithQuotaTag(PoolType, NumberOfBytes, Tag, _ReturnAddress());
}

static
PVOID
NTAPI
VfExAllocatePoolWithQuota(
    _In_ POOL_TYPE PoolType,
    _In_ SIZE_T NumberOfBytes)
{
    return VfpAllocateWithQuotaTag(PoolType, NumberOfBytes, TAG_NONE, _ReturnAddress());
}

static
PVOID
NTAPI
VfExAllocatePool2(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    PVOID Caller = _ReturnAddress();
    PVOID Block = VfpAllocatePool2(Flags, NumberOfBytes, Tag);

    if (Block == NULL)
        Block = ExAllocatePool2(Flags, NumberOfBytes, Tag);

    return VfpFinish(Block, NumberOfBytes, Tag, (BOOLEAN)((Flags & POOL_FLAG_PAGED) != 0), Caller);
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
    PVOID Caller = _ReturnAddress();
    PVOID Block = VfpAllocatePool2(Flags, NumberOfBytes, Tag);

    if (Block == NULL)
        Block = ExAllocatePool3(Flags, NumberOfBytes, Tag, ExtendedParameters, ExtendedParametersCount);

    return VfpFinish(Block, NumberOfBytes, Tag, (BOOLEAN)((Flags & POOL_FLAG_PAGED) != 0), Caller);
}

static
VOID
NTAPI
VfExFreePoolWithTag(
    _In_ PVOID P,
    _In_ ULONG Tag)
{
    if (P != NULL)
        VfpCheckFree(P);

    ExFreePoolWithTag(P, Tag);
}

static
VOID
NTAPI
VfExFreePool(
    _In_ PVOID P)
{
    if (P != NULL)
        VfpCheckFree(P);

    ExFreePool(P);
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
    { (ULONG_PTR)ExFreePoolWithTag, (ULONG_PTR)VfExFreePoolWithTag },
    { (ULONG_PTR)ExFreePool, (ULONG_PTR)VfExFreePool },
};

const ULONG VfPoolThunkCount = RTL_NUMBER_OF(VfPoolThunks);
