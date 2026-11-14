/*
 * PROJECT:     LiberNT Pool Allocator
 * FILE:        ntoskrnl/nvs/pool/include/poolspecial.h
 * PURPOSE:     Special pool definitions
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include "poolenv.h"

#define POOL_SPECIAL_ALIGNMENT          16
#define POOL_SPECIAL_SELECT_ALL         0x2A
#define POOL_SPECIAL_TAG_BITS           0x7FFFFFFFu

typedef enum _POOL_SPECIAL_STATUS
{
    PoolSpecialIntact = 0,
    PoolSpecialCorruptBefore,
    PoolSpecialCorruptAfter
} POOL_SPECIAL_STATUS;

typedef struct _POOL_SPECIAL_RECORD
{
    ULONG Tag;
    ULONG Size;
    ULONG Offset;
    ULONG Flags;
} POOL_SPECIAL_RECORD, *PPOOL_SPECIAL_RECORD;

BOOLEAN PoolSpecialSelect(_In_ ULONG Selector, _In_ SIZE_T Bytes, _In_ ULONG Tag);
BOOLEAN PoolSpecialPlace(_In_ SIZE_T Bytes, _In_ ULONG Alignment, _In_ BOOLEAN AtStart, _Out_ PULONG Offset);
PVOID PoolSpecialPrepare(_Out_ PPOOL_SPECIAL_RECORD Record, _Inout_ PVOID Page, _In_ SIZE_T Bytes, _In_ ULONG Offset,
                         _In_ ULONG Tag, _In_ ULONG Flags);
BOOLEAN PoolSpecialOwns(_In_ const POOL_SPECIAL_RECORD *Record, _In_ PVOID Page, _In_ PVOID Block);
POOL_SPECIAL_STATUS PoolSpecialCheck(_In_ const POOL_SPECIAL_RECORD *Record, _In_ PVOID Page,
                                     _Out_ ULONG_PTR *Corrupt);
