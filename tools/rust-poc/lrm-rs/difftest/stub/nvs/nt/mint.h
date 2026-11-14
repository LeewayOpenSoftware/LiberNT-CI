/* Host stand-in for nvs/nt/mint.h, for the LRM-RS FULL-STACK differential only (not kernel).
 * Provides exactly the NT types / constants / externs that the real ntspecial.c uses on the host.
 * The real mint.h pulls in <ntoskrnl.h> and the whole MM tree, which cannot build on the host;
 * this subset lets ntspecial.c compile UNMODIFIED against our mock MiGuard* + Ke* provider.
 * Our test-support file. Leeway Open Software. */
#pragma once

/* Base integer/pointer types + SAL + PAGE_SIZE/PAGE_SHIFT come from the pool host stand-in,
 * so there is a single definition shared with poolspecial.h / special.c. */
#include <nvs/pool/include/poolenv.h>

#include <stdint.h>

typedef uint64_t ULONG64, *PULONG64;
typedef long     NTSTATUS;
typedef unsigned char KIRQL;
typedef int      POOL_TYPE;
typedef void     VOID;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef NTAPI
#define NTAPI
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef UNICODE_NULL
#define UNICODE_NULL ((unsigned short)0)
#endif

#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)

/* NT IRQLs (architecture constants). */
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2

/* Bug-check codes used by ntspecial.c (public Windows values). */
#define SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION 0xC1u
#define PAGE_FAULT_IN_NONPAGED_AREA             0x50u

/* POOL_TYPE cache-aligned mask + NX bit: public NT POOL_TYPE enum / WDM (not in the private
 * headers). The full-stack differential feeds the identical value to the C and Rust sides, so the
 * equivalence proof does not depend on the exact number. */
#ifndef CACHE_ALIGNED_POOL_MASK
#define CACHE_ALIGNED_POOL_MASK 4
#endif
#ifndef POOL_NX_ALLOCATION
#define POOL_NX_ALLOCATION 0x200
#endif

/* MiGuard page-provider protection constants (values not in the private headers; the mock only
 * distinguishes executable vs not). */
#define MI_PROT_READWRITE         1u
#define MI_PROT_EXECUTE_READWRITE 2u

/* Guard-page provider classification (exact mirror of misys.h MI_GUARD_KIND). */
typedef enum _MI_GUARD_KIND
{
    MiGuardOutside = 0,
    MiGuardGuardPage,
    MiGuardLive,
    MiGuardFreed
} MI_GUARD_KIND;

/* Opaque-ish guard region: ntspecial.c only reads Region->Base (in a DbgPrint). The mock casts
 * this back to its full model. */
typedef struct _MI_GUARD_REGION
{
    ULONG64 Base;
} MI_GUARD_REGION, *PMI_GUARD_REGION;

/* Trap frame (opaque; only MiCheckSpecialPoolFault touches it and the harness never calls it). */
typedef struct _KTRAP_FRAME KTRAP_FRAME, *PKTRAP_FRAME;

/* A minimal MiSystem with the one sub-object whose address ntspecial.c takes. */
typedef struct _MI_SYSTEM_SPACE { int Dummy; } MI_SYSTEM_SPACE;
typedef struct _MI_SYSTEM { MI_SYSTEM_SPACE SystemSpace; } MI_SYSTEM;
extern MI_SYSTEM MiSystem;

/* Globals ntspecial.c references (defined in the harness). */
extern unsigned short MmVerifyDriverBuffer[];
extern ULONG64 MmNumberOfPhysicalPages;

/* Kernel services (mocked in the harness). */
unsigned long DbgPrint(const char *Format, ...);
KIRQL KeGetCurrentIrql(void);
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
void KeBugCheckWithTf(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4,
                      PKTRAP_FRAME Tf) __attribute__((noreturn));
ULONG_PTR KeGetTrapFramePc(PKTRAP_FRAME Tf);

/* MiGuard* provider (mocked in the harness; signatures match sysguard.c). */
NTSTATUS MiGuardRegionCreate(void *System, ULONG Slots, ULONG SlotPages, ULONG Reserve,
                             ULONG LowWater, ULONG64 Budget, SIZE_T ContextBytes,
                             PMI_GUARD_REGION *Region);
MI_GUARD_KIND MiGuardQuery(PMI_GUARD_REGION Region, ULONG64 VirtualAddress, PULONG64 DataVa,
                           PVOID *Context);
NTSTATUS MiGuardAllocate(PMI_GUARD_REGION Region, ULONG Protection, PULONG64 DataVa, PVOID *Context);
NTSTATUS MiGuardFree(PMI_GUARD_REGION Region, ULONG64 DataVa);
ULONG MiGuardRefill(PMI_GUARD_REGION Region);
int MiPtTranslate(void *Space, ULONG64 VirtualAddress, PULONG64 Physical, void *Unused);
