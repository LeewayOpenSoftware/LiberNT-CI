/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/include/misys.h
 * PURPOSE:     System virtual memory structures and interfaces
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <nvs/include/mm.h>

#define MI_SYSPTE_CLASSES         7
#define MI_SYSPTE_CLASS_MAX_PAGES (1u << (MI_SYSPTE_CLASSES - 1))
#define MI_SYSPTE_CACHE_DEPTH     16
#define MI_SYSPTE_CPU_CACHES      64
#define MI_SYSPTE_SPARE_TABLES    32

#define MI_KSTACK_CACHE_DEPTH     4

/* Reserve virtual capacity independently of RAM. Populate tables on demand,
 * apart from the initial paths needed to share kernel mappings across roots.
 * There is no arena-sized bitmap or full-arena table allocation.
 * This is an NVS layout choice, not a Windows implementation constant. */
#define MI_SYSPTE_64BIT_PAGES     (1ULL << (40 - PAGE_SHIFT))

typedef enum _MI_CACHE_TYPE
{
    MiCacheNone = 0,
    MiCacheFull,
    MiCacheWriteCombined
} MI_CACHE_TYPE;

typedef struct MI_CACHE_ALIGNED _MI_SYSPTE_CPU_CACHE
{
    MI_SPINLOCK Lock;
    ULONG Depth[MI_SYSPTE_CLASSES];
    ULONG64 Entry[MI_SYSPTE_CLASSES][MI_SYSPTE_CACHE_DEPTH];
    ULONG StackDepth;
    ULONG64 Stack[MI_KSTACK_CACHE_DEPTH];
    ULONG64 Reservations;
    ULONG64 CacheHits;
} MI_SYSPTE_CPU_CACHE, *PMI_SYSPTE_CPU_CACHE;

typedef struct _MI_SYSTEM_PTES
{
    PMI_SYSTEM System;
    PMI_VAD Vad;
    ULONG64 Base;
    ULONG64 PageCount;
    MI_SPINLOCK Lock;
    MI_VAD_ROOT Allocations;
    MI_SPINLOCK PopulateLock;
    ULONG SpareTableCount;
    ULONG SpareTable[MI_SYSPTE_SPARE_TABLES];
    volatile LONG64 FreePages;
    ULONG CacheCount;
    ULONG DefaultStackPages;
    MI_SYSPTE_CPU_CACHE Cache[MI_SYSPTE_CPU_CACHES];
} MI_SYSTEM_PTES, *PMI_SYSTEM_PTES;

typedef struct _MI_MDL
{
    PMI_ADDRESS_SPACE Space;
    ULONG64 StartVa;
    ULONG ByteOffset;
    ULONG ByteCount;
    ULONG PageCount;
    BOOLEAN Locked;
    BOOLEAN WriteAccess;
    ULONG64 MappedSystemVa;
    MI_FRAME_NUMBER Frames[1];
} MI_MDL, *PMI_MDL;

NTSTATUS MiSystemRegionCreate(_Inout_ PMI_SYSTEM System, _In_ ULONG64 PageCount, _In_ BOOLEAN PinTables,
                              _Out_ PMI_VAD *Vad, _Out_ PULONG64 Base);
VOID MiSystemRegionDelete(_Inout_ PMI_SYSTEM System, _Inout_ PMI_VAD Vad, _In_ BOOLEAN PinnedTables);
NTSTATUS MiSystemMapFrames(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress,
                           _In_ const MI_FRAME_NUMBER *Frames,
                           _In_ ULONG Count, _In_ ULONG Protection, _In_ ULONG LeafFlags, _In_ BOOLEAN OwnFrames);
VOID MiSystemUnmap(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count,
                   _In_ BOOLEAN FreeFrames);
VOID MiSystemUnmapLocal(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count);
NTSTATUS MiSystemCommitPages(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count,
                             _In_ ULONG Protection);
VOID MiSystemDecommitPages(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count);

NTSTATUS MiSystemCommitPinned(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count,
                              _In_ ULONG Protection);
VOID MiSystemDecommitPinned(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count);
NTSTATUS MiSystemProtect(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG Count,
                         _In_ ULONG Protection);

NTSTATUS MiSystemPtesInitialize(_Inout_ PMI_SYSTEM System, _In_ ULONG64 PageCount, _In_ ULONG CpuCount);
VOID MiSystemPtesUninitialize(_Inout_ PMI_SYSTEM System);
ULONG MiSystemPtesRefillTables(_Inout_ PMI_SYSTEM System);
ULONG64 MiReserveSystemPtes(_Inout_ PMI_SYSTEM System, _In_ ULONG PageCount);
NTSTATUS MiReserveSystemPtesEx(_Inout_ PMI_SYSTEM System, _In_ ULONG PageCount, _Out_ PULONG64 Base);
ULONG64 MiSystemPteCacheHits(_In_ PMI_SYSTEM System);
VOID MiReleaseSystemPtes(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG PageCount);

NTSTATUS MiCreateKernelStack(_Inout_ PMI_SYSTEM System, _In_ ULONG ReservePages, _In_ ULONG CommitPages,
                             _Out_ PULONG64 StackTop);
VOID MiDeleteKernelStack(_Inout_ PMI_SYSTEM System, _In_ ULONG64 StackTop, _In_ ULONG ReservePages);
NTSTATUS MiGrowKernelStack(_Inout_ PMI_SYSTEM System, _In_ ULONG64 StackTop, _In_ ULONG ReservePages,
                           _In_ ULONG64 NewLimit);

NTSTATUS MiMapIoSpace(_Inout_ PMI_SYSTEM System, _In_ ULONG64 PhysicalAddress, _In_ ULONG64 Bytes,
                      _In_ MI_CACHE_TYPE CacheType, _In_ ULONG Protection, _Out_ PULONG64 VirtualAddress);
VOID MiUnmapIoSpace(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress, _In_ ULONG64 Bytes);

NTSTATUS MiAllocateContiguousMemory(_Inout_ PMI_SYSTEM System, _In_ ULONG64 Bytes, _In_ ULONG64 LowestAddress,
                                    _In_ ULONG64 HighestAddress, _In_ ULONG64 BoundaryBytes,
                                    _In_ MI_CACHE_TYPE CacheType, _In_ ULONG Protection,
                                    _Out_ PULONG64 VirtualAddress);
NTSTATUS MiAllocateNonCachedMemory(_Inout_ PMI_SYSTEM System, _In_ ULONG64 Bytes, _Out_ PULONG64 VirtualAddress);
VOID MiFreeContiguousMemory(_Inout_ PMI_SYSTEM System, _In_ ULONG64 VirtualAddress);

NTSTATUS MiLockPages(_Inout_ PMI_ADDRESS_SPACE Space, _In_ ULONG64 StartVa, _In_ ULONG PageCount,
                     _In_ BOOLEAN UserMode, _In_ BOOLEAN WriteAccess, _Out_ PMI_FRAME_NUMBER Frames);
NTSTATUS MiLockSelectedPages(_Inout_ PMI_ADDRESS_SPACE Space, _In_ const ULONG64 *Addresses,
                             _In_ ULONG PageCount, _In_ BOOLEAN UserMode, _In_ BOOLEAN WriteAccess,
                             _Out_ PMI_FRAME_NUMBER Frames);
VOID MiUnlockFrames(_Inout_ PMI_SYSTEM System, _In_ const MI_FRAME_NUMBER *Frames, _In_ ULONG PageCount,
                    _In_ BOOLEAN WriteAccess);
NTSTATUS MiMapFrames(_Inout_ PMI_SYSTEM System, _In_ const MI_FRAME_NUMBER *Frames, _In_ ULONG PageCount,
                     _In_ MI_CACHE_TYPE CacheType, _In_ ULONG Protection, _Out_ PULONG64 Base);
VOID MiUnmapFrames(_Inout_ PMI_SYSTEM System, _In_ ULONG64 Base, _In_ ULONG PageCount);
PMI_MDL MiMdlAllocate(_In_ PMI_ADDRESS_SPACE Space, _In_ ULONG64 VirtualAddress, _In_ ULONG ByteCount);
VOID MiMdlFree(_Inout_ PMI_MDL Mdl);
NTSTATUS MiProbeAndLockPages(_Inout_ PMI_MDL Mdl, _In_ BOOLEAN UserMode, _In_ BOOLEAN WriteAccess);
NTSTATUS MiProbeAndLockPagesForCopy(_Inout_ PMI_MDL Mdl, _In_ BOOLEAN UserMode, _In_ BOOLEAN WriteAccess);
NTSTATUS MiMapPagesForCopy(_Inout_ PMI_MDL Mdl, _In_ BOOLEAN UserMode, _In_ BOOLEAN WriteAccess);
VOID MiUnlockPages(_Inout_ PMI_MDL Mdl);
NTSTATUS MiBuildMdlForSystemRange(_Inout_ PMI_MDL Mdl);
NTSTATUS MiMapLockedPages(_Inout_ PMI_MDL Mdl, _In_ MI_CACHE_TYPE CacheType, _Out_ PULONG64 SystemVa);
VOID MiUnmapLockedPages(_Inout_ PMI_MDL Mdl);
VOID MiUnmapLockedPagesLocal(_Inout_ PMI_MDL Mdl);
ULONG64 MiGetPhysicalAddress(_In_ PMI_ADDRESS_SPACE Space, _In_ ULONG64 VirtualAddress);

/* Guarded region: each slot is a guard page then DataPages data pages, plus one trailing guard page */
typedef enum _MI_GUARD_KIND
{
    MiGuardOutside = 0,
    MiGuardGuardPage,
    MiGuardLive,
    MiGuardFreed
} MI_GUARD_KIND;

typedef struct _MI_GUARD_STATS
{
    ULONG SlotCount;
    ULONG LiveSlots;
    ULONG ReadySlots;
    ULONG FreeSlots;
    ULONG64 CommittedPages;
    ULONG64 PeakCommittedPages;
    ULONG64 Allocations;
    ULONG64 Frees;
    ULONG64 Failures;
} MI_GUARD_STATS, *PMI_GUARD_STATS;

typedef struct _MI_GUARD_REGION
{
    PMI_SYSTEM System;
    PMI_VAD Vad;
    ULONG64 Base;
    ULONG64 End;
    ULONG SlotCount;
    ULONG DataPages;
    ULONG SlotPages;
    ULONG ReserveTarget;
    ULONG ReuseDelay;
    ULONG ContextBytes;
    ULONG64 BudgetPages;
    MI_SPINLOCK Lock;
    LONG *State;
    PULONG Queue;
    ULONG QueueHead;
    ULONG QueueCount;
    PULONG Ready;
    ULONG ReadyCount;
    ULONG LiveCount;
    PUCHAR Context;
    ULONG64 CommittedPages;
    ULONG64 PeakCommittedPages;
    ULONG64 Allocations;
    ULONG64 Frees;
    ULONG64 Failures;
} MI_GUARD_REGION, *PMI_GUARD_REGION;

NTSTATUS MiGuardRegionCreate(_Inout_ PMI_SYSTEM System, _In_ ULONG SlotCount, _In_ ULONG DataPages,
                             _In_ ULONG ReservePages, _In_ ULONG ReuseDelay, _In_ ULONG64 BudgetPages,
                             _In_ ULONG ContextBytes, _Out_ PMI_GUARD_REGION *Region);
VOID MiGuardRegionDelete(_Inout_ PMI_GUARD_REGION Region);
NTSTATUS MiGuardAllocate(_Inout_ PMI_GUARD_REGION Region, _Out_ PULONG64 DataVa, _Out_opt_ PVOID *Context);
NTSTATUS MiGuardFree(_Inout_ PMI_GUARD_REGION Region, _In_ ULONG64 DataVa);
MI_GUARD_KIND MiGuardQuery(_In_ PMI_GUARD_REGION Region, _In_ ULONG64 VirtualAddress, _Out_opt_ PULONG64 DataVa,
                           _Out_opt_ PVOID *Context);
ULONG MiGuardRefill(_Inout_ PMI_GUARD_REGION Region);
VOID MiGuardQueryStats(_Inout_ PMI_GUARD_REGION Region, _Out_ PMI_GUARD_STATS Stats);
