/*
 * PROJECT:     LiberNT Pool Allocator
 * FILE:        ntoskrnl/nvs/pool/special/special.c
 * PURPOSE:     Special pool: selection, placement in the guarded page, fill pattern
 *
 * SPDX-FileCopyrightText: 2026 Leeway Open Software
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "../include/poolspecial.h"

static
UCHAR
PoolSpecialPattern(_In_ PVOID Page)
{
    return (UCHAR)((((ULONG_PTR)Page >> PAGE_SHIFT) * 37u + 0x5Bu) | 1u);
}

BOOLEAN
PoolSpecialSelect(
    _In_ ULONG Selector,
    _In_ SIZE_T Bytes,
    _In_ ULONG Tag)
{
    ULONG Position;

    if (Selector == 0 || Bytes == 0 || Bytes >= PAGE_SIZE)
        return FALSE;

    if (Selector == POOL_SPECIAL_SELECT_ALL)
        return TRUE;

    if (Selector < PAGE_SIZE)
    {
        return ((Bytes + POOL_SPECIAL_ALIGNMENT - 1) & ~(SIZE_T)(POOL_SPECIAL_ALIGNMENT - 1)) ==
               (((SIZE_T)Selector + POOL_SPECIAL_ALIGNMENT - 1) & ~(SIZE_T)(POOL_SPECIAL_ALIGNMENT - 1));
    }

    Selector &= POOL_SPECIAL_TAG_BITS;
    Tag &= POOL_SPECIAL_TAG_BITS;
    for (Position = 0; Position < 4; Position++)
    {
        UCHAR Wanted = (UCHAR)(Selector >> (Position * 8));
        UCHAR Actual = (UCHAR)(Tag >> (Position * 8));

        if (Wanted == '*')
            return TRUE;

        if (Wanted != '?' && Wanted != Actual)
            return FALSE;
    }

    return TRUE;
}

BOOLEAN
PoolSpecialPlace(
    _In_ SIZE_T Bytes,
    _In_ ULONG Alignment,
    _In_ BOOLEAN AtStart,
    _Out_ PULONG Offset)
{
    SIZE_T Aligned;

    *Offset = 0;
    if (Bytes == 0 || Bytes >= PAGE_SIZE)
        return FALSE;

    if (Alignment < POOL_SPECIAL_ALIGNMENT || (Alignment & (Alignment - 1)) != 0 || Alignment > PAGE_SIZE)
        Alignment = POOL_SPECIAL_ALIGNMENT;

    Aligned = (Bytes + Alignment - 1) & ~(SIZE_T)(Alignment - 1);
    if (!AtStart)
        *Offset = (ULONG)(PAGE_SIZE - Aligned);

    return TRUE;
}

PVOID
PoolSpecialPrepare(
    _Out_ PPOOL_SPECIAL_RECORD Record,
    _Inout_ PVOID Page,
    _In_ SIZE_T Bytes,
    _In_ ULONG Offset,
    _In_ ULONG Tag,
    _In_ ULONG Flags)
{
    RtlFillMemory(Page, PAGE_SIZE, PoolSpecialPattern(Page));
    Record->Tag = Tag;
    Record->Size = (ULONG)Bytes;
    Record->Offset = Offset;
    Record->Flags = Flags;
    return (PUCHAR)Page + Offset;
}

BOOLEAN
PoolSpecialOwns(
    _In_ const POOL_SPECIAL_RECORD *Record,
    _In_ PVOID Page,
    _In_ PVOID Block)
{
    return (BOOLEAN)(Record->Size != 0 && (PUCHAR)Block == (PUCHAR)Page + Record->Offset);
}

POOL_SPECIAL_STATUS
PoolSpecialCheck(
    _In_ const POOL_SPECIAL_RECORD *Record,
    _In_ PVOID Page,
    _Out_ ULONG_PTR *Corrupt)
{
    UCHAR Pattern = PoolSpecialPattern(Page);
    const UCHAR *Bytes = Page;
    ULONG Position;

    *Corrupt = 0;
    if (Record->Offset > PAGE_SIZE || Record->Size > PAGE_SIZE - Record->Offset)
    {
        *Corrupt = (ULONG_PTR)Page;
        return PoolSpecialCorruptBefore;
    }

    for (Position = 0; Position < Record->Offset; Position++)
    {
        if (Bytes[Position] != Pattern)
        {
            *Corrupt = (ULONG_PTR)&Bytes[Position];
            return PoolSpecialCorruptBefore;
        }
    }

    for (Position = Record->Offset + Record->Size; Position < PAGE_SIZE; Position++)
    {
        if (Bytes[Position] != Pattern)
        {
            *Corrupt = (ULONG_PTR)&Bytes[Position];
            return PoolSpecialCorruptAfter;
        }
    }

    return PoolSpecialIntact;
}
