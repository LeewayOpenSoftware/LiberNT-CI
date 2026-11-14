/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/nt/lrmrs.c
 * PURPOSE:     LRM-RS in-kernel integration: MiGuard vtable shim + Rust-vs-C self-test
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experimental, gated by LIBERNT_LRM_RS (CMake LIBERNT_ENABLE_LRM_RS, default OFF). When OFF this
 * translation unit defines nothing and ntoskrnl is unchanged. When ON it exposes the real MiGuard
 * region to the Rust special-pool orchestration (lrm_mm_*) as a vtable of C-ABI callbacks, and a
 * self-test that drives alloc/free/corruption through both the Rust path and the C path/engine over
 * a private region and compares them in lockstep. The self-test never bug-checks (clean, PASSIVE).
 */

#include <nvs/nt/mint.h>
#include <nvs/pool/ex/expool.h>
#include <nvs/pool/include/poolspecial.h>
#include <nvs/include/lrmrs.h>

/* FFI layout invariants shared with the Rust side (mm.rs / lib.rs const asserts). Compile-time: a
 * mismatch fails the build rather than corrupting a bug check, a vtable call, or a block record.
 * Checked in both the flag-ON and flag-OFF builds (and the nvs_neutral standalone object). */
C_ASSERT(sizeof(LRM_BUGCHECK) == 5 * sizeof(ULONG_PTR));
C_ASSERT(FIELD_OFFSET(LRM_BUGCHECK, P1) == 1 * sizeof(ULONG_PTR));
C_ASSERT(FIELD_OFFSET(LRM_BUGCHECK, P2) == 2 * sizeof(ULONG_PTR));
C_ASSERT(FIELD_OFFSET(LRM_BUGCHECK, P3) == 3 * sizeof(ULONG_PTR));
C_ASSERT(FIELD_OFFSET(LRM_BUGCHECK, P4) == 4 * sizeof(ULONG_PTR));
C_ASSERT(sizeof(POOL_SPECIAL_RECORD) == 16);
C_ASSERT(sizeof(LRM_GUARD_VTBL) == 5 * sizeof(PVOID));
C_ASSERT(MiGuardOutside == 0);
C_ASSERT(MiGuardGuardPage == 1);
C_ASSERT(MiGuardLive == 2);
C_ASSERT(MiGuardFreed == 3);

#if defined(LIBERNT_LRM_RS)

/* --- MiGuard provider exposed to the Rust orchestration (This = PMI_GUARD_REGION). --- */

static int
LrmGuardQuery(void *This, ULONG_PTR Addr, ULONG_PTR *OutPage, void **OutCtx)
{
    ULONG64 Page = 0;
    PVOID Ctx = NULL;
    MI_GUARD_KIND Kind = MiGuardQuery((PMI_GUARD_REGION)This, (ULONG64)Addr, &Page, &Ctx);

    if (OutPage != NULL)
        *OutPage = (ULONG_PTR)Page;
    if (OutCtx != NULL)
        *OutCtx = Ctx;
    return (int)Kind;
}

static UCHAR
LrmGuardAllocate(void *This, UCHAR Executable, ULONG_PTR *OutPage, void **OutCtx)
{
    ULONG64 Page = 0;
    PVOID Ctx = NULL;
    ULONG Protection = Executable ? MI_PROT_EXECUTE_READWRITE : MI_PROT_READWRITE;

    if (!NT_SUCCESS(MiGuardAllocate((PMI_GUARD_REGION)This, Protection, &Page, &Ctx)))
        return 0;

    if (OutPage != NULL)
        *OutPage = (ULONG_PTR)Page;
    if (OutCtx != NULL)
        *OutCtx = Ctx;
    return 1;
}

static UCHAR
LrmGuardFree(void *This, ULONG_PTR Page)
{
    return (UCHAR)(NT_SUCCESS(MiGuardFree((PMI_GUARD_REGION)This, (ULONG64)Page)) ? 1 : 0);
}

static void
LrmGuardRefill(void *This)
{
    MiGuardRefill((PMI_GUARD_REGION)This);
}

VOID
LrmRsBuildGuardVtbl(PMI_GUARD_REGION Region, PLRM_GUARD_VTBL Vtbl)
{
    Vtbl->Query = LrmGuardQuery;
    Vtbl->Allocate = LrmGuardAllocate;
    Vtbl->Free = LrmGuardFree;
    Vtbl->Refill = LrmGuardRefill;
    Vtbl->This = Region;
}

/* --- Self-test --- */

#define LRM_ST_SLOTS        64
#define LRM_ST_RESERVE      8
#define LRM_ST_BUDGET       64
#define LRM_ST_CACHE_MASK   4u    /* NonPagedPoolCacheAligned bit (public POOL_TYPE) */
#define LRM_ST_CACHE_ALIGN  64u   /* MI_SPECIAL_POOL_CACHE_ALIGNMENT */

typedef struct _LRM_ST_CASE
{
    SIZE_T Bytes;
    ULONG Tag;
    ULONG PoolType;
    ULONG SpecialType;  /* 0 = page end, 1 = page start, 2 = default (underruns=FALSE here) */
} LRM_ST_CASE;

static const LRM_ST_CASE LrmStCases[] =
{
    {   32, 'tseT', 0x000, 0 },  /* NonPagedPool (NX clear -> executable), page end */
    {  200, 'tseT', 0x001, 1 },  /* PagedPool, page start */
    { 4000, 'tseT', 0x200, 2 },  /* NonPagedPoolNx, default placement */
    {   64, 'tseT', 0x004, 0 },  /* cache-aligned, page end */
};

/*
 * Returns STATUS_SUCCESS iff every scenario agrees between Rust and C. *Detail carries the number
 * of scenarios on success, or (scenario*100 + step) on the first disagreement.
 */
NTSTATUS
NTAPI
LrmRsSelfTest(PULONG Detail)
{
    PMI_GUARD_REGION Region = NULL;
    LRM_GUARD_VTBL Vtbl;
    NTSTATUS Status;
    ULONG i;
    ULONG Fail = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        if (Detail != NULL)
            *Detail = 0xE0;
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Every C entry into Rust is gated on LrmRsPanicked (Dwight review SHOULD 1): once the Rust side
     * has fatally stopped, do not re-enter it, even on this test's own private region. No hang risk
     * today (a panic bug-checks the box), but the invariant holds uniformly. */
    if (LrmRsPanicked)
    {
        if (Detail != NULL)
            *Detail = 0xE2;
        return STATUS_INVALID_DEVICE_STATE;
    }

    Status = MiGuardRegionCreate(&MiSystem, LRM_ST_SLOTS, 1, LRM_ST_RESERVE, 0, LRM_ST_BUDGET,
                                 sizeof(POOL_SPECIAL_RECORD), &Region);
    if (!NT_SUCCESS(Status) || Region == NULL)
    {
        if (Detail != NULL)
            *Detail = 0xE1;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    LrmRsBuildGuardVtbl(Region, &Vtbl);

    for (i = 0; i < RTL_NUMBER_OF(LrmStCases); i++)
    {
        const LRM_ST_CASE *Case = &LrmStCases[i];
        ULONG Base = (i + 1) * 100;
        ULONG Alignment = (Case->PoolType & LRM_ST_CACHE_MASK) ? LRM_ST_CACHE_ALIGN : 0;
        BOOLEAN AtStart = (Case->SpecialType == 0) ? FALSE : (Case->SpecialType == 1) ? TRUE : FALSE;
        ULONG RustOffset = 0;
        ULONG COffset = 0;
        LRM_BUGCHECK Bc;
        PVOID Rp = NULL;
        UCHAR Rc;
        ULONG64 RpPage = 0;
        ULONG64 PageR = 0;
        PVOID CtxR = NULL;
        ULONG_PTR Corr = 0;
        ULONG_PTR CCorr = 0;
        ULONG RustCheck;
        POOL_SPECIAL_STATUS CCheck;
        PUCHAR Pg;
        ULONG Idx;
        ULONG GoodIdx;
        UCHAR Good;
        PVOID Cp;

        /* (1) placement agreement (pure engine) */
        if ((rust_pool_special_place(Case->Bytes, Alignment, (UCHAR)AtStart, &RustOffset) != 0) !=
            (PoolSpecialPlace(Case->Bytes, Alignment, AtStart, &COffset) != FALSE) ||
            RustOffset != COffset)
        {
            Fail = Base + 1;
            break;
        }

        /* (2) selector agreement (pure engine) */
        if ((rust_pool_special_select(POOL_SPECIAL_SELECT_ALL, Case->Bytes, Case->Tag) != 0) !=
            (PoolSpecialSelect(POOL_SPECIAL_SELECT_ALL, Case->Bytes, Case->Tag) != FALSE))
        {
            Fail = Base + 2;
            break;
        }

        /* (3) Rust allocate over the real MiGuard region */
        RtlZeroMemory(&Bc, sizeof(Bc));
        Rc = lrm_mm_allocate_special_pool(&Vtbl, 1, 0, Case->Bytes, Case->Tag, Case->PoolType,
                                          Case->SpecialType, 0, &Rp, &Bc);
        if (Rc != 1 || Rp == NULL)
        {
            Fail = Base + 3;
            break;
        }
        /* The allocation is now LIVE; free it on every failure path below so the region has no live
         * slot at MiGuardRegionDelete (sysguard.c MI_ASSERT(LiveCount == 0); leak on a free build). */
        RpPage = (ULONG64)((ULONG_PTR)Rp & ~(ULONG_PTR)(PAGE_SIZE - 1));

        /* (4) placed at the C-computed in-page offset */
        if (((ULONG_PTR)Rp & (PAGE_SIZE - 1)) != COffset)
        {
            MiGuardFree(Region, RpPage);
            Fail = Base + 4;
            break;
        }

        /* (5) the region sees it live, with a record context */
        if (MiGuardQuery(Region, (ULONG64)(ULONG_PTR)Rp, &PageR, &CtxR) != MiGuardLive || CtxR == NULL)
        {
            MiGuardFree(Region, RpPage);
            Fail = Base + 5;
            break;
        }

        /* (6) ownership agreement */
        if ((rust_pool_special_owns(CtxR, (PVOID)(ULONG_PTR)PageR, Rp) != 0) !=
            (PoolSpecialOwns((const POOL_SPECIAL_RECORD *)CtxR, (PVOID)(ULONG_PTR)PageR, Rp) != FALSE))
        {
            MiGuardFree(Region, RpPage);
            Fail = Base + 6;
            break;
        }

        /* (7) clean fill-pattern check agreement */
        RustCheck = (ULONG)rust_pool_special_check(CtxR, (PVOID)(ULONG_PTR)PageR, &Corr);
        CCheck = PoolSpecialCheck((const POOL_SPECIAL_RECORD *)CtxR, (PVOID)(ULONG_PTR)PageR, &CCorr);
        if (RustCheck != (ULONG)CCheck || Corr != CCorr || CCheck != PoolSpecialIntact)
        {
            MiGuardFree(Region, RpPage);
            Fail = Base + 7;
            break;
        }

        /* (8) corruption classification agreement over the live page */
        Pg = (PUCHAR)(ULONG_PTR)PageR;
        Idx = (COffset == 0) ? (PAGE_SIZE - 1) : 0;      /* a guard byte either side of the block */
        GoodIdx = (COffset == 0) ? (PAGE_SIZE - 2) : 1;
        Good = Pg[GoodIdx];
        Pg[Idx] = (UCHAR)(Good ^ 0xFF);                   /* pattern is odd; xor 0xFF differs */

        RustCheck = (ULONG)rust_pool_special_check(CtxR, (PVOID)(ULONG_PTR)PageR, &Corr);
        CCheck = PoolSpecialCheck((const POOL_SPECIAL_RECORD *)CtxR, (PVOID)(ULONG_PTR)PageR, &CCorr);
        Pg[Idx] = Good;                                    /* restore so the free is clean */
        if (RustCheck != (ULONG)CCheck || Corr != CCorr || CCheck == PoolSpecialIntact)
        {
            MiGuardFree(Region, RpPage);                    /* byte restored above; slot still live */
            Fail = Base + 8;
            break;
        }

        /* (9) Rust free (clean) */
        RtlZeroMemory(&Bc, sizeof(Bc));
        if (lrm_mm_free_special_pool(&Vtbl, (ULONG_PTR)Rp, 0, &Bc) != 0)
        {
            MiGuardFree(Region, RpPage);                    /* free did not complete; release the slot */
            Fail = Base + 9;
            break;
        }

        /* (10/11) C oracle allocate/free over the same region, same placement */
        Cp = MiSpecialPoolAllocateC(Region, FALSE, Case->Bytes, Case->Tag, (POOL_TYPE)Case->PoolType,
                                    Case->SpecialType, 0);
        if (Cp == NULL)
        {
            Fail = Base + 10;
            break;
        }
        if (((ULONG_PTR)Cp & (PAGE_SIZE - 1)) != COffset)
        {
            MiSpecialPoolFreeC(Region, Cp, 0);
            Fail = Base + 11;
            break;
        }
        MiSpecialPoolFreeC(Region, Cp, 0);
    }

    MiGuardRegionDelete(Region);

    if (Fail != 0)
    {
        if (Detail != NULL)
            *Detail = Fail;
        return STATUS_UNSUCCESSFUL;
    }

    if (Detail != NULL)
        *Detail = RTL_NUMBER_OF(LrmStCases);
    return STATUS_SUCCESS;
}

/*
 * Set before the panic bug check; the ntspecial.c special-pool wrappers read it and, once set, fail
 * the allocation (caller falls back to the normal pool) and ignore frees - no allocator runs on the
 * Rust region again, so the bug check's own machinery never re-takes a lock the panicking thread may
 * hold (Dwight review P3; god 2026-11-14). Written with InterlockedExchange8 and declared volatile so
 * a CPU already inside a wrapper observes it with acquire ordering (acquire on x86/x64).
 */
BOOLEAN volatile LrmRsPanicked = FALSE;

/*
 * LRM-RS panic policy target. The Rust panic handler (lib.rs on_panic) and the lrm_fatal! forwarder
 * reach here; we flag that Rust has stopped and raise the controlled LEEWAY_RUST_PANIC bug check with
 * the source location. This is the one place that knows the numeric code + parameter layout. The code
 * is FIXED (reason is carried in P1, never ORed into the code - Dwight review P2: triage and the
 * bugcodes.mc registration key on the single value). It never returns (KeBugCheckEx does not), does
 * not unwind (the Rust side is panic = "abort"), allocates nothing, and is safe at any IRQL: File,
 * when non-NULL, is a &'static str in the image's read-only data. See LRM-RS-PANIC-POLICY.md.
 *
 * Parameters: P1 = Reason (0x01 a real panic got through; 0x02 Rust chose to stop), P2 = File ptr,
 * P3 = Line, P4 = FileLen.
 */
DECLSPEC_NORETURN VOID
LrmRsFatalStop(ULONG Reason, const UCHAR *File, SIZE_T FileLen, ULONG Line)
{
    InterlockedExchange8((CHAR volatile *)&LrmRsPanicked, (CHAR)TRUE);
    KeBugCheckEx(LEEWAY_RUST_PANIC,
                 (ULONG_PTR)Reason,
                 (ULONG_PTR)File,
                 (ULONG_PTR)Line,
                 (ULONG_PTR)FileLen);
}

#if defined(LIBERNT_LRM_RS_FORCE_PANIC_TEST)
/* Exported LAB trigger (lnrspoc resolves it). Mode 0 = plain Rust panic; Mode 1 = panic while holding
 * the real special-pool region lock; Mode 2 = that plus a concurrent SMP2 special-pool workload. The
 * region-lock modes delegate to ntspecial.c, which owns the static region. Never returns. The lnrspoc
 * driver supplies the DISPATCH_LEVEL and driver-spin-lock variants itself (Mode 0 under those). */
DECLSPEC_NORETURN VOID NTAPI
LrmRsForcePanic(ULONG Mode)
{
    if (Mode == 1)
        MiSpecialPoolForcePanic(0);   /* hold the region lock, no concurrent load */
    else if (Mode >= 2)
        MiSpecialPoolForcePanic(1);   /* hold the region lock + concurrent SMP2 load */

    lrm_rs_force_panic();             /* Mode 0, and the unreachable tail of the above */
}
#endif

#endif /* LIBERNT_LRM_RS */
