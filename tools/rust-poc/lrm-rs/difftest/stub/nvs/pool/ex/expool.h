/* Host stand-in for nvs/pool/ex/expool.h, for the LRM-RS FULL-STACK differential only.
 * The real expool.h pulls in poolheap.h/pooltrack.h/poolback.h and a large EX_POOL_STATE; this
 * subset reproduces ONLY what ntspecial.c uses, verbatim where it matters (EX_POOL_* macros +
 * ExpPoolHeapFromType), so ntspecial.c compiles UNMODIFIED with identical decode semantics.
 * POOL_TYPE / POOL_NX_ALLOCATION come from mint.h (included first by ntspecial.c).
 * Our test-support file. Leeway Open Software. */
#pragma once

#define EX_POOL_HEAP_NONPAGED     0
#define EX_POOL_HEAP_PAGED        1
#define EX_POOL_HEAP_EXECUTABLE   2
#define EX_POOL_BASE_TYPE_MASK    1
#define EX_POOL_FLAG_SPECIAL_POOL 0x20

extern ULONG ExpPoolFlags;

/* Verbatim from the real expool.h (the C under test depends on this inline). */
static inline ULONG
ExpPoolHeapFromType(POOL_TYPE PoolType)
{
    if (PoolType & EX_POOL_BASE_TYPE_MASK)
        return EX_POOL_HEAP_PAGED;
    return (PoolType & POOL_NX_ALLOCATION) ? EX_POOL_HEAP_NONPAGED : EX_POOL_HEAP_EXECUTABLE;
}
