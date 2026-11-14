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
 * ABI: every Rust export and every callback type is extern "C", so on x64 and ARM64 the names are
 * undecorated and on i386 they are cdecl with a leading underscore - the C prototypes here must stay
 * plain (no NTAPI/FASTCALL). Only LrmRsSelfTest/LrmRsForcePanic are NTAPI, and those are C.
 *
 * Floating-point/SSE: lrm_rs targets x86_64-pc-windows-gnu for GCC/Clang builds and
 * x86_64-pc-windows-msvc for MSVC builds (SSE2 permitted; x87/MMX/AVX rejected by
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

/*
 * Kernel panic policy (LRM-RS-PANIC-POLICY.md, owner-approved 2026-11-14). The Rust panic handler
 * (lib.rs) forwards here on an unexpected panic — which the panic-free discipline and the FORBID_PANIC
 * build gate are meant to make unreachable. This raises ONE controlled bug check with the FIXED code
 * LEEWAY_RUST_PANIC (reason in P1, never ORed into the code) - a unique registered Leeway value with
 * no Windows overlap, distinct from the Verifier (0xC4/0xC9) and Special Pool (0xC1/0xD6) codes. It
 * never returns; it does not unwind (panic = "abort"); it does no allocation. Safe at any IRQL
 * (including HIGH_LEVEL): File is a &'static str in the image's read-only data (never pageable;
 * path-remapped to /rust/...). Parameters: P1 = Reason (0x01 real panic, 0x02 deliberate stop via
 * lrm_fatal!), P2 = File ptr, P3 = Line, P4 = FileLen. The name has no "panic" substring.
 *
 * The code value LEEWAY_RUST_PANIC (0x4C524D00) is the single generated symbol from
 * sdk/include/reactos/mc/bugcodes.mc (reachable here via <ntoskrnl.h>); it is NOT re-#defined to
 * avoid a duplicate, divergent definition (Dwight review).
 */
DECLSPEC_NORETURN VOID LrmRsFatalStop(ULONG Reason, const UCHAR *File, SIZE_T FileLen, ULONG Line);

/* Set TRUE (InterlockedExchange8) by LrmRsFatalStop before KeBugCheckEx. Once set, the ntspecial.c
 * special-pool wrappers FAIL the allocation (caller falls back to the normal pool) and IGNORE frees -
 * no allocator runs on the Rust region again, so the bug check's own machinery never re-takes a lock
 * the panicking thread may hold (Dwight review P3; god 2026-11-14). Volatile: acquire-ordered reads on
 * x86/x64, so a CPU already inside a wrapper observes it. */
extern BOOLEAN volatile LrmRsPanicked;

#if defined(LIBERNT_LRM_RS_FORCE_PANIC_TEST)
/* LAB ONLY (feature `panic-test`): validate the panic policy in a disposable VM. All never return.
 * lrm_rs_force_panic: the Rust deliberate panic (-> LrmRsFatalStop reason 0x01).
 * MiSpecialPoolForcePanic(ConcurrentLoad): panic while holding the REAL special-pool region lock
 *   (ntspecial.c owns the static region); ConcurrentLoad != 0 also runs an SMP2 special-pool workload.
 * LrmRsForcePanic(Mode): exported trigger lnrspoc resolves via MmGetSystemRoutineAddress -
 *   Mode 0 = plain; 1 = hold region lock; 2 = hold region lock + concurrent SMP2 load. */
DECLSPEC_NORETURN VOID lrm_rs_force_panic(VOID);
DECLSPEC_NORETURN VOID MiSpecialPoolForcePanic(ULONG ConcurrentLoad);
DECLSPEC_NORETURN VOID NTAPI LrmRsForcePanic(ULONG Mode);
#endif
#endif
