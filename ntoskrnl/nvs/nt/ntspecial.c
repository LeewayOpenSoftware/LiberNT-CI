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
#include <nvs/include/lrmrs.h>

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

/*
 * The C special-pool allocate orchestration, parameterised by the guard region and the underruns
 * mode so it can serve both the live path (MmAllocateSpecialPool) and the LRM-RS self-test oracle
 * over a private region. Always compiled: the normal build runs it; the flag-ON build keeps it as
 * the differential oracle and the fallback. Mirrors the contract exactly (0xC1 sub-codes/params).
 */
PVOID
NTAPI
MiSpecialPoolAllocateC(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ BOOLEAN Underruns,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ POOL_TYPE PoolType,
    _In_ ULONG SpecialType,
    _In_ KIRQL Irql)
{
    BOOLEAN AtStart = (SpecialType == 0) ? FALSE : (SpecialType == 1) ? TRUE : Underruns;
    BOOLEAN Paged = (BOOLEAN)(((ULONG)PoolType & EX_POOL_BASE_TYPE_MASK) != 0);
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

/*
 * The C special-pool free orchestration, parameterised by the guard region (oracle/fallback;
 * always compiled). Same order and 0xC1 sub-codes/params as the live path.
 */
VOID
NTAPI
MiSpecialPoolFreeC(
    _Inout_ PMI_GUARD_REGION Region,
    _In_ PVOID P,
    _In_ KIRQL Irql)
{
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

/*
 * Public special-pool allocate. With LIBERNT_LRM_RS the Rust orchestration (lrm_mm_*) runs over the
 * real MiGuard region and we raise the returned controlled bug check; otherwise the C body runs.
 * No default changes: the flag is opt-in and OFF by default.
 */
PVOID
NTAPI
MmAllocateSpecialPool(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ POOL_TYPE PoolType,
    _In_ ULONG SpecialType)
{
#if defined(LIBERNT_LRM_RS)
    LRM_GUARD_VTBL Vtbl;
    LRM_BUGCHECK Bc;
    PVOID Out = NULL;
    UCHAR Rc;

    /* Once the Rust side has fatally stopped, NO allocator runs on its region again: fail the request
     * so the caller falls back to the normal pool. The C oracle must NOT serve it - it has no record of
     * the Rust-issued slots on this region (double hand-out / corrupt free / nested 0xC1), and it would
     * re-take a lock the panicking thread may still hold. LrmRsPanicked is volatile (acquire-ordered on
     * x86/x64), set with InterlockedExchange8 before the bug check (Dwight review P3; god 2026-11-14). */
    if (LrmRsPanicked)
        return NULL;

    if (MiSpecialPoolRegion == NULL)
        return NULL;

    LrmRsBuildGuardVtbl(MiSpecialPoolRegion, &Vtbl);
    Rc = lrm_mm_allocate_special_pool(&Vtbl, 1, (UCHAR)MiSpecialPoolUnderruns, NumberOfBytes, Tag,
                                      (ULONG)PoolType, SpecialType, (UCHAR)KeGetCurrentIrql(), &Out, &Bc);
    if (Rc == 2)
        KeBugCheckEx(Bc.Code, Bc.P1, Bc.P2, Bc.P3, Bc.P4);

    return Out;
#else
    return MiSpecialPoolAllocateC(MiSpecialPoolRegion, MiSpecialPoolUnderruns, NumberOfBytes, Tag,
                                  PoolType, SpecialType, KeGetCurrentIrql());
#endif
}

/*
 * Public special-pool free. With LIBERNT_LRM_RS the Rust orchestration runs over the real MiGuard
 * and raises the returned controlled bug check; otherwise the C body runs.
 */
VOID
NTAPI
MmFreeSpecialPool(
    _In_ PVOID P)
{
#if defined(LIBERNT_LRM_RS)
    LRM_GUARD_VTBL Vtbl;
    LRM_BUGCHECK Bc;

    /* Once the Rust side has fatally stopped, the free is IGNORED - no allocator runs on the Rust
     * region again. The C oracle must NOT free a Rust-issued block (it would corrupt or raise a nested
     * 0xC1, or hang on a held lock). Leaking the block is the safe choice inside a bug check. */
    if (LrmRsPanicked)
        return;

    /* No NULL-region early return here: with no special pool, MiGuardQuery(NULL, ...) returns
     * MiGuardOutside, so the Rust path stops with 0xC1/0x20 - exactly as the C path does. (The
     * allocate side returns NULL on a NULL region, which is its correct C behaviour.) */
    LrmRsBuildGuardVtbl(MiSpecialPoolRegion, &Vtbl);
    if (lrm_mm_free_special_pool(&Vtbl, (ULONG_PTR)P, (UCHAR)KeGetCurrentIrql(), &Bc) == 1)
        KeBugCheckEx(Bc.Code, Bc.P1, Bc.P2, Bc.P3, Bc.P4);
#else
    MiSpecialPoolFreeC(MiSpecialPoolRegion, P, KeGetCurrentIrql());
#endif
}

#if defined(LIBERNT_LRM_RS) && defined(LIBERNT_LRM_RS_FORCE_PANIC_TEST)
/* LAB-ONLY force-panic orchestration. Lives here because it needs the file-static special-pool region
 * (MiSpecialPoolRegion) and the same-file MmAllocate/FreeSpecialPool (neither is exported). It
 * validates the panic policy end to end in a disposable VM: a Rust panic while the real special-pool
 * region lock is held must still STOP (0x4C524D00, P1=1) without a hang - because LrmRsFatalStop sets
 * LrmRsPanicked, after which this region never serves another allocation (allocate -> NULL, free
 * ignored), so the bug check's own pool use can never re-take the held lock. */

/* Concurrent special-pool workload for the SMP2 mode: on another CPU, keep allocating/freeing special
 * pool so the region lock is genuinely contended when the trigger thread panics holding it. Never
 * returns meaningfully (the box bug-checks); no cleanup needed. */
static volatile LONG MiSpecialPoolPanicLoadCount;   /* bumped each iteration by the load thread */

static VOID NTAPI
MiSpecialPoolPanicLoad(_In_opt_ PVOID Context)
{
    /* Pin to a processor OTHER than the trigger's (passed as Context), so mode 3 is genuine cross-CPU
     * contention rather than possibly co-scheduled on the trigger's CPU (Stanley SHOULD 1). On a
     * single-processor box there is no other CPU, so we run wherever we are. */
    ULONG TriggerCpu = (ULONG)(ULONG_PTR)Context;
    KAFFINITY Active = KeQueryActiveProcessors();
    KAFFINITY Other = Active & ~(((KAFFINITY)1) << TriggerCpu);
    if (Other != 0)
        KeSetSystemAffinityThreadEx(Other);

    for (;;)
    {
        PVOID P = MmAllocateSpecialPool(64, 'LPsT', NonPagedPool, 0);
        if (P != NULL)
            MmFreeSpecialPool(P);
        InterlockedIncrement(&MiSpecialPoolPanicLoadCount);
    }
}

/* ConcurrentLoad != 0 -> first start the SMP2 workload above, pinned to another CPU, and wait until it
 * has actually allocated before taking the lock. Then acquire the real region lock and trigger the Rust
 * panic while holding it. Never returns. */
DECLSPEC_NORETURN VOID
MiSpecialPoolForcePanic(_In_ ULONG ConcurrentLoad)
{
    KIRQL OldIrql = PASSIVE_LEVEL;

    if (ConcurrentLoad != 0)
    {
        HANDLE Thread = NULL;
        OBJECT_ATTRIBUTES Attr;
        ULONG TriggerCpu = KeGetCurrentProcessorNumber();
        ULONG Spins;

        MiSpecialPoolPanicLoadCount = 0;
        InitializeObjectAttributes(&Attr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &Attr, NULL, NULL,
                                            MiSpecialPoolPanicLoad, (PVOID)(ULONG_PTR)TriggerCpu)) && Thread != NULL)
            ZwClose(Thread);

        /* Bounded wait until the load thread's allocation counter moves, so we take the lock with real
         * concurrent contention and not before it has started (Stanley SHOULD 1). ~1 s cap. */
        for (Spins = 0; MiSpecialPoolPanicLoadCount == 0 && Spins < 1000; Spins++)
        {
            LARGE_INTEGER Delay;
            Delay.QuadPart = -1 * 1000 * 10;   /* 1 ms, relative */
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }
    }

    if (MiSpecialPoolRegion != NULL)
    {
        MI_SPIN_ACQUIRE(&MiSpecialPoolRegion->Lock, &OldIrql);   /* never released: we panic holding it */
    }
    else
    {
        /* Special pool is off: there is no region lock to hold, so modes 2/3 are NOT the lock-held
         * case. Say so loudly so the lab cannot record a false PASS (Stanley SHOULD 2). */
        DbgPrint("Driver Verifier: LRM-RS force-panic REGION LOCK NOT HELD (special pool is off)\n");
    }

    (VOID)OldIrql;
    lrm_rs_force_panic();   /* never returns: LrmRsFatalStop -> KeBugCheckEx(0x4C524D00, 1, ...) */
}
#endif

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
