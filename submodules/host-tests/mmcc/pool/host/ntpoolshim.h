/*
 * PROJECT:     ReactOS host-native tests
 * FILE:        submodules/host-tests/mmcc/pool/host/ntpoolshim.h
 * PURPOSE:     NT pool allocator host-test compatibility definitions
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <nvs/pool/include/poolenv.h>

typedef ULONG POOL_TYPE, EX_POOL_PRIORITY;
typedef ULONG64 POOL_FLAGS;
typedef enum _POOL_EXTENDED_PARAMETER_TYPE
{
    PoolExtendedParameterInvalidType = 0,
    PoolExtendedParameterPriority,
    PoolExtendedParameterSecurePool,
    PoolExtendedParameterNumaNode,
    PoolExtendedParameterMax
} POOL_EXTENDED_PARAMETER_TYPE;
typedef struct _POOL_EXTENDED_PARAMETER
{
    struct
    {
        ULONG64 Type : 8;
        ULONG64 Optional : 1;
        ULONG64 Reserved : 64 - 9;
    };
    union
    {
        ULONG64 Reserved2;
        PVOID Reserved3;
        ULONG Priority;
        ULONG PreferredNode;
    };
} POOL_EXTENDED_PARAMETER;
typedef const POOL_EXTENDED_PARAMETER *PCPOOL_EXTENDED_PARAMETER;
typedef struct _SHIM_DISPATCHER_HEADER { UCHAR Type; } SHIM_DISPATCHER_HEADER;
typedef struct _SHIM_KPROCESS { SHIM_DISPATCHER_HEADER Header; } SHIM_KPROCESS;
typedef struct _EPROCESS { SHIM_KPROCESS Pcb; SIZE_T Quota[2]; LONG References; } EPROCESS, *PEPROCESS, *PKPROCESS;
typedef union _SHIM_LARGE_INTEGER { struct { ULONG LowPart; LONG HighPart; }; long long QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
#define KOBJECT_TYPE_MASK 0x7F
#define ProcessObject 3
#define BAD_POOL_HEADER 0x19
#ifndef MEMORY_ALLOCATION_ALIGNMENT
#define MEMORY_ALLOCATION_ALIGNMENT 16
#endif
extern PVOID MmSystemRangeStart;
BOOLEAN MmIsAddressValid(PVOID Address);
LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER Frequency);
ULONG RtlRandomEx(PULONG Seed);

#define IN
#define OUT
#define NTAPI
#define CODE_SEG(s)
#define DBG 1
#define DPRINT1(...) ((void)0)
#define NonPagedPool 0
#define NonPagedPoolExecute 0
#define PagedPool 1
#define NonPagedPoolNx 0x200
#define CACHE_ALIGNED_POOL_MASK 4
#define MUST_SUCCEED_POOL_MASK 2
#define POOL_QUOTA_FAIL_INSTEAD_OF_RAISE 8
#define POOL_RAISE_IF_ALLOCATION_FAILURE 0x10
#define POOL_NX_ALLOCATION 0x200
#define POOL_ZERO_ALLOCATION 0x400
#define POOL_FLAG_USE_QUOTA 1ULL
#define POOL_FLAG_UNINITIALIZED 2ULL
#define POOL_FLAG_SESSION 4ULL
#define POOL_FLAG_CACHE_ALIGNED 8ULL
#define POOL_FLAG_RAISE_ON_FAILURE 0x20ULL
#define POOL_FLAG_NON_PAGED 0x40ULL
#define POOL_FLAG_NON_PAGED_EXECUTE 0x80ULL
#define POOL_FLAG_PAGED 0x100ULL
#define POOL_FLAG_RESERVED2 0x200ULL
#define POOL_FLAG_RESERVED3 0x400ULL
#define POOL_FLAG_SPECIAL_POOL 0x100000000ULL
#define POOL_FLAG_REQUIRED_MASK 0xFFFFFFFFULL
#define STATUS_COMMITMENT_LIMIT ((NTSTATUS)0xC000012D)
#define MUST_SUCCEED_POOL_EMPTY 0x41
#define BAD_POOL_CALLER 0xC2
#define TAG_NONE 0x656e6f4e
#define APC_LEVEL 1
#define MI_PROT_READWRITE 4
#define MI_PROT_EXECUTE_READWRITE 6
#define InterlockedCompareExchange(p, v, c) __sync_val_compare_and_swap(p, c, v)
#define InterlockedExchange(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)

extern ULONG KeNumberProcessors;
#define KiNumberProcessors KeNumberProcessors
extern PEPROCESS PsInitialSystemProcess;
extern int MiSystem;

ULONG64 KeQueryInterruptTime(VOID);
KIRQL KeGetCurrentIrql(VOID);
_Noreturn VOID KeBugCheckEx(ULONG Code, ULONG_PTR A, ULONG_PTR B, ULONG_PTR C, ULONG_PTR D);
_Noreturn VOID ExRaiseStatus(NTSTATUS Status);
BOOLEAN MmUseSpecialPool(SIZE_T Size, ULONG Tag);
PVOID MmAllocateSpecialPool(SIZE_T Size, ULONG Tag, POOL_TYPE Type, ULONG Mode);
BOOLEAN MmIsSpecialPoolAddress(PVOID Block);
VOID MmFreeSpecialPool(PVOID Block);
PEPROCESS PsGetCurrentProcess(VOID);
NTSTATUS PsChargeProcessPoolQuota(PEPROCESS Process, ULONG Type, SIZE_T Size);
VOID PsReturnPoolQuota(PEPROCESS Process, ULONG Type, SIZE_T Size);
VOID ObReferenceObject(PEPROCESS Process);
VOID ObDereferenceObject(PEPROCESS Process);
NTSTATUS MiSystemCommitPinned(int *System, ULONG64 Base, ULONG Pages, ULONG Protection);
VOID MiSystemDecommitPinned(int *System, ULONG64 Base, ULONG Pages);
VOID MiWakeBalanceSetManager(VOID);
NTSTATUS MiWaitForMemory(NTSTATUS Status, PULONG Attempts);
VOID InitializePool(POOL_TYPE Type, ULONG Threshold);
PVOID ExAllocatePoolWithTag(POOL_TYPE Type, SIZE_T Size, ULONG Tag);
PVOID ExAllocatePool2(POOL_FLAGS Flags, SIZE_T Size, ULONG Tag);
PVOID ExAllocatePool3(POOL_FLAGS Flags, SIZE_T Size, ULONG Tag,
                     PCPOOL_EXTENDED_PARAMETER Parameters, ULONG Count);
PVOID ExAllocatePoolWithQuotaTag(POOL_TYPE Type, SIZE_T Size, ULONG Tag);
VOID ExFreePoolWithTag(PVOID Block, ULONG Tag);
VOID ExReturnPoolQuota(PVOID Block);
