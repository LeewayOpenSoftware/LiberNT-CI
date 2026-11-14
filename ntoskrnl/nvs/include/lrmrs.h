/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/include/lrmrs.h
 * PURPOSE:     LRM-RS (Rust special pool) in-kernel integration: FFI surface, C oracle, self-test
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experimental. The whole Rust path is gated by LIBERNT_LRM_RS (CMake option
 * LIBERNT_ENABLE_LRM_RS, default OFF). When OFF the special pool stays pure C and ntoskrnl is
 * unchanged. The C bodies (MiSpecialPool*C) are always compiled: the normal build runs them, and
 * the flag-ON build keeps them as the differential oracle and the fallback.
 *
 * Floating-point/SSE: lrm_rs targets x86_64-pc-windows-gnu (SSE2 permitted; x87/MMX/AVX rejected by
 * the staticlib gate in sdk/cmake/rust-build.cmake), so the page fill/compare may use the volatile
 * XMM registers. That is safe at any IRQL here, including DISPATCH_LEVEL on the routed
 * MmAllocate/FreeSpecialPool: the trap entry preserves the volatile XMM0..Xmm5 in the trap frame
 * (KTRAP_FRAME, sdk/include/ndk/amd64/ketypes.h:447; saved by ntoskrnl/ke/amd64/trap.S).
 */

#pragma once

/*
 * The MiGuard page provider exposed to the Rust orchestration as a vtable of C-ABI callbacks,
 * matching `LrmGuardVtbl` in tools/rust-poc/lrm-rs/src/mm.rs. The Rust side calls these over the
 * REAL MiGuard region passed as `This`.
 */
typedef struct _LRM_GUARD_VTBL
{
    /* query(This, Addr, *OutPage, *OutCtx) -> MI_GUARD_KIND */
    int (*Query)(void *This, ULONG_PTR Addr, ULONG_PTR *OutPage, void **OutCtx);
    /* allocate(This, Executable, *OutPage, *OutCtx) -> 1 ok / 0 fail */
    UCHAR (*Allocate)(void *This, UCHAR Executable, ULONG_PTR *OutPage, void **OutCtx);
    /* free(This, Page) -> 1 ok / 0 fail */
    UCHAR (*Free)(void *This, ULONG_PTR Page);
    /* refill(This) */
    void (*Refill)(void *This);
    /* opaque provider state passed back as the first argument */
    void *This;
} LRM_GUARD_VTBL, *PLRM_GUARD_VTBL;

/*
 * A pending controlled bug check returned by value from the Rust path (never a direct trap),
 * matching `BugCheck` in mm.rs: `{ u32 code; usize p1..p4 }` with repr(C). The kernel shim turns
 * it into KeBugCheckEx(Code, P1, P2, P3, P4).
 */
typedef struct _LRM_BUGCHECK
{
    ULONG Code;
    ULONG_PTR P1;
    ULONG_PTR P2;
    ULONG_PTR P3;
    ULONG_PTR P4;
} LRM_BUGCHECK, *PLRM_BUGCHECK;

/* --- Rust C-ABI exports from the lrm_rs staticlib (no_mangle). Referenced only under the flag. --- */

/* special-pool engine (lib.rs) */
UCHAR rust_pool_special_select(ULONG Selector, SIZE_T Bytes, ULONG Tag);
UCHAR rust_pool_special_place(SIZE_T Bytes, ULONG Alignment, UCHAR AtStart, ULONG *Offset);
PVOID rust_pool_special_prepare(void *Record, PVOID Page, SIZE_T Bytes, ULONG Offset, ULONG Tag, ULONG Flags);
UCHAR rust_pool_special_owns(const void *Record, PVOID Page, PVOID Block);
LONG rust_pool_special_check(const void *Record, PVOID Page, ULONG_PTR *Corrupt);

/* orchestration (mm.rs). alloc rc: 0 = declined (*OutPtr), 1 = allocated (*OutPtr), 2 = bug check
 * (*OutBc). free rc: 0 = freed, 1 = bug check (*OutBc). */
UCHAR lrm_mm_allocate_special_pool(const LRM_GUARD_VTBL *Vtbl, UCHAR RegionPresent, UCHAR Underruns,
                                   SIZE_T Bytes, ULONG Tag, ULONG PoolType, ULONG SpecialType, UCHAR Irql,
                                   PVOID *OutPtr, PLRM_BUGCHECK OutBc);
UCHAR lrm_mm_free_special_pool(const LRM_GUARD_VTBL *Vtbl, ULONG_PTR P, UCHAR Irql, PLRM_BUGCHECK OutBc);

/* --- C special-pool orchestration as a region-parameterised oracle/fallback (ntspecial.c). Always
 * compiled: the normal build calls these; the flag-ON build keeps them as oracle + fallback and the
 * self-test drives them over a private test region. They raise 0xC1 like the live path. --- */
struct _MI_GUARD_REGION;
PVOID NTAPI MiSpecialPoolAllocateC(struct _MI_GUARD_REGION *Region, BOOLEAN Underruns, SIZE_T NumberOfBytes,
                                   ULONG Tag, POOL_TYPE PoolType, ULONG SpecialType, KIRQL Irql);
VOID NTAPI MiSpecialPoolFreeC(struct _MI_GUARD_REGION *Region, PVOID P, KIRQL Irql);

#if defined(LIBERNT_LRM_RS)
/* Build a vtable over the real MiGuard `Region`. */
VOID LrmRsBuildGuardVtbl(struct _MI_GUARD_REGION *Region, PLRM_GUARD_VTBL Vtbl);

/*
 * In-kernel Rust-vs-C self-test over a private MiGuard region: alloc/free/corrupt classification
 * through both the Rust path and the C path/engine, compared in lockstep. Never bug-checks (clean,
 * PASSIVE_LEVEL). STATUS_SUCCESS iff all scenarios agree; *Detail = scenarios run (success) or the
 * failing scenario/step code (failure). Exported ONLY in a flag-ON build (conditional spec entry);
 * lnrspoc resolves it with MmGetSystemRoutineAddress.
 */
NTSTATUS NTAPI LrmRsSelfTest(PULONG Detail);
#endif
