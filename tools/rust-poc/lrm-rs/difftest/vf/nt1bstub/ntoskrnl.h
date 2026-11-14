/* Host stand-in for <ntoskrnl.h>, for the LRM-RS Driver-Verifier Stage 1b FULL-STACK differential.
 * Provides exactly what ntoskrnl/nvs/nt/ntdrvmgmt.c uses so it compiles UNMODIFIED on the host. The
 * harness drives MmAddVerifierThunks / MmIsDriverVerifying / MmIsVerifierEnabled /
 * MiInitializeDriverVerifier; MiVerifyLoadedImage/MiRedirectVerifierThunk/MiIsVerifiedDriverName are
 * compiled but not called (they only need to link). Values (STATUS_*, verifier flags,
 * LDRP_IMAGE_VERIFYING) follow public NT and are fed identically to C and Rust.
 * Our test-support file. Leeway Open Software. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef int32_t       NTSTATUS, LONG, *PLONG, LOGICAL;
typedef void          VOID;
typedef uint8_t       BOOLEAN, UCHAR;
typedef uint8_t       KIRQL;
typedef uint16_t      USHORT, WCHAR;
typedef uint32_t      ULONG, *PULONG;
typedef int64_t       LONGLONG;
typedef void         *PVOID;
typedef char         *PCHAR;
typedef WCHAR        *PWCHAR, *PWSTR, *PWCH;
typedef uintptr_t     ULONG_PTR, SIZE_T;
typedef int           POOL_TYPE;
typedef int           KWAIT_REASON, KPROCESSOR_MODE;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define NTAPI
#define FASTCALL
#define IN
#define OUT
#define OPTIONAL
#define _In_
#define _Inout_
#define _Out_
#define _In_opt_
#define _Out_opt_

#define PAGED_CODE() ((void)0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define ASSERT(x) ((void)0)
#define UNIMPLEMENTED ((void)0)
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define UNICODE_NULL ((WCHAR)0)
#define REG_NONE 0

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000)
#define STATUS_NOT_SUPPORTED          ((NTSTATUS)0xC00000BB)
#define STATUS_INVALID_PARAMETER_1    ((NTSTATUS)0xC00000EF)
#define STATUS_INVALID_PARAMETER_2    ((NTSTATUS)0xC00000F0)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)

#define DRIVER_VERIFIER_SPECIAL_POOLING       0x0001u
#define DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS 0x0008u
#define DRIVER_VERIFIER_IO_CHECKING           0x0010u
#define DRIVER_VERIFIER_DEADLOCK_DETECTION    0x0020u
#define LDRP_IMAGE_VERIFYING 0x00020000u

#define PagedPool 1
#define MUTANT_INCREMENT 1
#define WrVirtualMemory 0
#define KernelMode 0
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1

#define RtlCopyMemory(d, s, n) memcpy((d), (s), (size_t)(n))
#define InterlockedIncrement(p) (++(*(p)))
#define KeEnterCriticalRegion() ((void)0)
#define KeLeaveCriticalRegion() ((void)0)

typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
#define InitializeListHead(h) ((h)->Flink = (h)->Blink = (h))
#define InsertTailList(h, e) do { PLIST_ENTRY h_ = (h), e_ = (e), b_ = h_->Blink; \
    e_->Flink = h_; e_->Blink = b_; b_->Flink = e_; h_->Blink = e_; } while (0)
#define CONTAINING_RECORD(addr, type, field) \
    ((type *)((char *)(addr) - (char *)(&((type *)0)->field)))
#define RTL_CONSTANT_STRING(s) { (USHORT)(sizeof(s) - sizeof((s)[0])), (USHORT)sizeof(s), (PWSTR)(s) }

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _KMUTANT { int Dummy; } KMUTANT;

typedef struct _LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;   /* PsLoadedModuleList link (boot walk iterates this) */
    PVOID DllBase;
    ULONG SizeOfImage;
    ULONG Flags;
    UNICODE_STRING BaseDllName;
    PVOID LoadedImports;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

/* Boot-driver list (LoaderBlock->BootDriverListHead) entry. Minimal: the boot walk only reads Link
 * (via CONTAINING_RECORD) and LdrEntry->DllBase. */
typedef struct _BOOT_DRIVER_LIST_ENTRY {
    LIST_ENTRY Link;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
} BOOT_DRIVER_LIST_ENTRY, *PBOOT_DRIVER_LIST_ENTRY;

typedef struct _LOADER_PARAMETER_BLOCK {
    LIST_ENTRY BootDriverListHead;
} LOADER_PARAMETER_BLOCK, *PLOADER_PARAMETER_BLOCK;

typedef struct _DRIVER_OBJECT { PVOID DriverSection; } DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _MM_DRIVER_VERIFIER_DATA { ULONG Level; ULONG Loads; } MM_DRIVER_VERIFIER_DATA;

typedef struct _VF_THUNK { ULONG_PTR Pristine; ULONG_PTR Replacement; } VF_THUNK;

typedef struct _DRIVER_VERIFIER_THUNK_PAIRS {
    PVOID PristineRoutine;
    PVOID NewRoutine;
} DRIVER_VERIFIER_THUNK_PAIRS, *PDRIVER_VERIFIER_THUNK_PAIRS;

typedef struct _DRIVER_SPECIFIED_VERIFIER_THUNKS {
    LIST_ENTRY ListEntry;
    PLDR_DATA_TABLE_ENTRY DataTableEntry;
    ULONG NumberOfThunks;
} DRIVER_SPECIFIED_VERIFIER_THUNKS, *PDRIVER_SPECIFIED_VERIFIER_THUNKS;

typedef struct _IMAGE_IMPORT_DESCRIPTOR { ULONG Name; ULONG FirstThunk; } IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;
typedef struct _IMAGE_THUNK_DATA { union { ULONG_PTR Function; } u1; } IMAGE_THUNK_DATA, *PIMAGE_THUNK_DATA;

/* Services (defined in the harness or stubbed there). */
unsigned long DbgPrint(const char *Format, ...);
PVOID ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
void  ExFreePoolWithTag(PVOID P, ULONG Tag);
NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PVOID Timeout);
LONG KeReleaseMutant(KMUTANT *Mutant, LONG Increment, BOOLEAN Abandon, BOOLEAN Wait);
PLDR_DATA_TABLE_ENTRY NTAPI MiLookupDataTableEntry(IN PVOID Address);
BOOLEAN MiIsSpecialPoolEnabled(void);
void VfInitializePoolTracking(void);
BOOLEAN RtlEqualUnicodeString(PCUNICODE_STRING String1, PCUNICODE_STRING String2, BOOLEAN CaseInSensitive);
PVOID NTAPI RtlImageDirectoryEntryToData(PVOID Base, BOOLEAN MappedAsImage, USHORT Directory, ULONG *Size);

/* Verifier thunk tables referenced by MiVerifyLoadedImage (compiled, not called here). */
extern const VF_THUNK VfPoolThunks[];
extern const ULONG VfPoolThunkCount;
extern const VF_THUNK VfIrqlThunks[];
extern const ULONG VfIrqlThunkCount;
extern const VF_THUNK VfIoThunks[];
extern const ULONG VfIoThunkCount;

/* Globals the boot walk reads; defined in the harness. */
extern LIST_ENTRY PsLoadedModuleList;
extern ULONG_PTR PsNtosImageBase;
