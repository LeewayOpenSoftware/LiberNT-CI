/* Host stand-in for <ntoskrnl.h>, for the LRM-RS Driver-Verifier Stage 5 FULL-STACK differential.
 * Provides exactly what ntoskrnl/vf/vfpool.c uses so it compiles UNMODIFIED on the host; the real
 * header is the whole kernel SDK. The pool/Mm/Ke services and MmVerifierData are defined in the
 * harness. POOL_TYPE/POOL_FLAGS/CACHE_ALIGNED_POOL_MASK values follow public NT/WDM and are fed
 * identically to the C and Rust sides (equivalence is value-independent of them).
 * Our test-support file. Leeway Open Software. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef long          NTSTATUS;
typedef void          VOID;
typedef uint8_t       BOOLEAN, UCHAR, KIRQL;
typedef uint16_t      USHORT, WCHAR;
typedef uint32_t      ULONG, *PULONG;
typedef int32_t       LONG, *PLONG;
typedef int64_t       LONGLONG;
typedef uint64_t      ULONGLONG;
typedef void         *PVOID;
typedef char         *PCHAR;
typedef WCHAR        *PWSTR, *PWCH;
typedef uintptr_t     ULONG_PTR, SIZE_T;
typedef int           POOL_TYPE;
typedef uint64_t      POOL_FLAGS;
typedef int           EX_POOL_PRIORITY;
typedef ULONG_PTR     KSPIN_LOCK, *PKSPIN_LOCK;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define NTAPI
#define FORCEINLINE static inline
#define _In_
#define _In_opt_
#define _Inout_
#define _Out_
#define _In_reads_opt_(x)

#define PAGE_SHIFT 12
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2
#define DRIVER_VERIFIER_DETECTED_VIOLATION 0xC4u
#define DRIVER_VERIFIER_SPECIAL_POOLING 0x1u
#define LDRP_IMAGE_VERIFYING 0x00020000u
#define LowPoolPrioritySpecialPoolOverrun  8
#define LowPoolPrioritySpecialPoolUnderrun 9

/* POOL_TYPE (public NT): NonPagedPool/Execute = 0, PagedPool = 1, NonPagedPoolNx = 0x200. */
#define NonPagedPool         0
#define NonPagedPoolExecute  0
#define PagedPool            1
#define NonPagedPoolNx       0x200
#define CACHE_ALIGNED_POOL_MASK 4
#define TAG_NONE 0

/* POOL_FLAGS (public WDM): only the bits vfpool.c consults. */
#define POOL_ZERO_ALLOCATION            0x00000040u   /* POOL_TYPE flag (ExAllocate* path) */
#define POOL_QUOTA_FAIL_INSTEAD_OF_RAISE 0x00000008u
#define POOL_FLAG_UNINITIALIZED         0x0000000000000002ULL
#define POOL_FLAG_CACHE_ALIGNED         0x0000000000000008ULL
#define POOL_FLAG_NON_PAGED             0x0000000000000040ULL
#define POOL_FLAG_NON_PAGED_EXECUTE     0x0000000000000080ULL
#define POOL_FLAG_PAGED                 0x0000000000000100ULL

#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define _ReturnAddress() __builtin_return_address(0)
#define RtlZeroMemory(d, l) memset((d), 0, (size_t)(l))
#define InterlockedIncrement(p) (++(*(p)))
#define UNICODE_NULL ((WCHAR)0)

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
#define InitializeListHead(h) ((h)->Flink = (h)->Blink = (h))
#define InsertHeadList(h, e) do { PLIST_ENTRY h_ = (h), e_ = (e), f_ = h_->Flink; \
    e_->Flink = f_; e_->Blink = h_; f_->Blink = e_; h_->Flink = e_; } while (0)
#define RemoveEntryList(e) do { PLIST_ENTRY e_ = (e), f_ = e_->Flink, b_ = e_->Blink; \
    b_->Flink = f_; f_->Blink = b_; } while (0)
#define CONTAINING_RECORD(addr, type, field) \
    ((type *)((char *)(addr) - (char *)(&((type *)0)->field)))

typedef struct _LDR_DATA_TABLE_ENTRY {
    PVOID DllBase;
    ULONG SizeOfImage;
    ULONG Flags;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

typedef struct _MM_DRIVER_VERIFIER_DATA {
    ULONG Level;
    ULONG UnTrackedPool;
    ULONG PagedBytes;
    ULONG NonPagedBytes;
    ULONG PeakPagedBytes;
    ULONG PeakNonPagedBytes;
    ULONG CurrentPagedPoolAllocations;
    ULONG CurrentNonPagedPoolAllocations;
    ULONG PeakPagedPoolAllocations;
    ULONG PeakNonPagedPoolAllocations;
    ULONG AllocationsAttempted;
    ULONG AllocationsSucceeded;
    ULONG AllocationsFailed;
    ULONG AllocationsWithNoTag;
    ULONG AllocationsSucceededSpecialPool;
    ULONG Loads;
} MM_DRIVER_VERIFIER_DATA;
extern MM_DRIVER_VERIFIER_DATA MmVerifierData;

typedef struct _VF_THUNK { ULONG_PTR Pristine; ULONG_PTR Replacement; } VF_THUNK;
typedef struct _POOL_EXTENDED_PARAMETER POOL_EXTENDED_PARAMETER;
typedef const POOL_EXTENDED_PARAMETER *PCPOOL_EXTENDED_PARAMETER;

/* Spin lock: single-threaded host, no-ops. */
#define KeInitializeSpinLock(l) ((void)(*(l) = 0))
#define KeAcquireSpinLock(l, oi) ((void)(*(oi) = 0))
#define KeReleaseSpinLock(l, oi) ((void)0)

/* Kernel services (defined in the harness). */
KIRQL KeGetCurrentIrql(void);
void KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
    __attribute__((noreturn));
unsigned long DbgPrint(const char *Format, ...);

PVOID ExAllocatePool(POOL_TYPE PoolType, SIZE_T NumberOfBytes);
PVOID ExAllocatePoolWithQuota(POOL_TYPE PoolType, SIZE_T NumberOfBytes);
PVOID ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
PVOID ExAllocatePoolWithQuotaTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
PVOID ExAllocatePoolWithTagPriority(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag, EX_POOL_PRIORITY Priority);
PVOID ExAllocatePool2(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag);
PVOID ExAllocatePool3(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag, PCPOOL_EXTENDED_PARAMETER ExtendedParameters, ULONG ExtendedParametersCount);
void  ExFreePoolWithTag(PVOID P, ULONG Tag);
void  ExFreePool(PVOID P);
PVOID MmAllocateSpecialPool(SIZE_T NumberOfBytes, ULONG Tag, POOL_TYPE PoolType, ULONG SpecialType);
