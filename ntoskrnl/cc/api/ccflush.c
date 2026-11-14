/*
 * PROJECT:     LiberNT Cache Manager
 * FILE:        ntoskrnl/cc/api/ccflush.c
 * PURPOSE:     Cache flushing and dirty-data writeback interfaces
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ccnt.h"

VOID
NTAPI
CcFlushCache(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _Out_opt_ PIO_STATUS_BLOCK IoStatus)
{
    PCC_NT_MAP NtMap = CcNtReferenceMap(SectionObjectPointer);
    ULONG64 Offset = (FileOffset != NULL) ? (ULONG64)FileOffset->QuadPart : 0;
    ULONG64 Bytes = (FileOffset != NULL) ? Length : (ULONG64)-1;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Flushed = 0;

    if (NtMap != NULL)
    {
        Status = CcNtFlushMap(NtMap, Offset, Bytes, ~0u, &Flushed);

        if (NT_SUCCESS(Status))
        {
            ULONG64 SegmentBytes = (Bytes == (ULONG64)-1)
                                       ? (ULONG64)MI_ATOMIC_READ64(&NtMap->Control->Segment->SizeInBytes) - Offset
                                       : Bytes;

            Status = MiSegmentFlush(NtMap->Control->Segment, Offset, SegmentBytes);
        }

        CcNtDereferenceMap(NtMap);
    }
    else
    {
        PMI_CONTROL_AREA Control = MiReferenceDataControlArea(SectionObjectPointer);

        if (Control != NULL)
        {
            ULONG64 Size = (ULONG64)MI_ATOMIC_READ64(&Control->Segment->SizeInBytes);

            if (Offset < Size)
                Status = MiSegmentFlush(Control->Segment, Offset, (Bytes == (ULONG64)-1) ? Size - Offset : Bytes);

            MiDereferenceControlArea(Control);
        }
    }

    if (IoStatus != NULL)
    {
        IoStatus->Status = Status;
        IoStatus->Information = (ULONG_PTR)Flushed << PAGE_SHIFT;
    }
}

BOOLEAN
NTAPI
CcPurgeCacheSection(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    PCC_NT_MAP NtMap = CcNtReferenceMap(SectionObjectPointer);
    ULONG64 Offset = (FileOffset != NULL) ? (ULONG64)FileOffset->QuadPart : 0;
    ULONG64 Bytes = (FileOffset != NULL && Length != 0) ? Length : 0;
    BOOLEAN Purged = TRUE;

    if (NtMap != NULL)
    {
        ULONG64 End = (Bytes == 0) ? (ULONG64)-1 : Offset + Bytes;

        if (CcBcbRangeBusy(&NtMap->Map, Offset, End))
        {
            CcNtDereferenceMap(NtMap);
            return FALSE;
        }

        CcMapDetachViews(&NtMap->Map, Offset, (Bytes == 0) ? (ULONG64)-1 : Bytes);
        Purged = MiSegmentPurge(NtMap->Control->Segment, Offset, Bytes, MiPurgeDiscard);
        if (Purged)
            CcDirtyDiscard(&NtMap->Map, Offset, (Bytes == 0) ? (ULONG64)-1 : Bytes);
        if (Purged && Offset == 0 && Bytes == 0 && (Flags & UNINITIALIZE_CACHE_MAPS))
            NtMap->PurgeOnClose = TRUE;
        CcNtDereferenceMap(NtMap);
    }
    else
    {
        PMI_CONTROL_AREA Control = MiReferenceDataControlArea(SectionObjectPointer);

        if (Control != NULL)
        {
            Purged = MiSegmentPurge(Control->Segment, Offset, Bytes, MiPurgeDiscard);
            if (Purged && Offset == 0 && Bytes == 0)
                MiSegmentDereferenceAndClose(Control->Segment);
            else
                MiDereferenceControlArea(Control);
        }
    }

    return Purged;
}

/*
 * Make the cache coherent with a non-cached write about to reach the disk:
 * flush the range, then (unless the caller asks not to) purge it so later
 * cached and mapped reads re-read the new data. The routine returns VOID; it
 * reports through IoStatus. Success is set only when coherency was actually
 * achieved, so a purge that leaves any page in range cached or mapped yields
 * STATUS_PURGE_FAILED (an existing, public status, which the one caller,
 * fastfat, already handles). See AUDIT-R1-IO-CC-FS.md section R2-A.1.
 */
VOID
NTAPI
CcCoherencyFlushAndPurgeCache(
    _In_ PSECTION_OBJECT_POINTERS SectionObjectPointer,
    _In_opt_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_opt_ ULONG Flags)
{
    CcFlushCache(SectionObjectPointer, FileOffset, Length, IoStatus);

    /* A failed flush is reported as-is; the data is not yet on disk, so
     * purging would discard it. */
    if (!NT_SUCCESS(IoStatus->Status))
        return;

    if (Flags & CC_FLUSH_AND_PURGE_NO_PURGE)
        return;

    /* Purge the just-flushed range. A pinned BCB or a surviving mapped page
     * makes this fail; coherency is then not guaranteed, so say so instead
     * of leaving the flush's STATUS_SUCCESS. */
    if (!CcPurgeCacheSection(SectionObjectPointer, FileOffset, Length, 0))
    {
        IoStatus->Status = STATUS_PURGE_FAILED;
        IoStatus->Information = 0;
    }
}
