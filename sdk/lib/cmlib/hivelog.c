/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Configuration Manager Library - Incremental hive log recovery
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "cmlib.h"
#define NDEBUG
#include <debug.h>

static
BOOLEAN
HvpIsLogBaseBlockValid(
    _In_ PHBASE_BLOCK BaseBlock)
{
    return BaseBlock->Signature == HV_HBLOCK_SIGNATURE &&
           BaseBlock->Major == HSYS_MAJOR &&
           BaseBlock->Type == HV_LOG_INCREMENTAL_TYPE &&
           BaseBlock->Format == HBASE_FORMAT_MEMORY &&
           BaseBlock->Sequence1 == BaseBlock->Sequence2 &&
           BaseBlock->Length && !(BaseBlock->Length % HBLOCK_SIZE) &&
           HvpHiveHeaderChecksum(BaseBlock) == BaseBlock->CheckSum;
}

static
PHV_LOG_ENTRY
HvpGetLogEntry(
    _In_ PUCHAR Log,
    _In_ ULONG LogSize,
    _In_ ULONG Offset,
    _In_ ULONG Sequence)
{
    PHV_LOG_ENTRY Entry;
    PHV_LOG_DIRTY_PAGE Page;
    ULONG DataSize, i;

    if (Offset > LogSize || LogSize - Offset < sizeof(HV_LOG_ENTRY))
        return NULL;

    Entry = (PHV_LOG_ENTRY)(Log + Offset);
    if (Entry->Signature != HV_LOG_ENTRY_SIGNATURE ||
        Entry->Sequence != Sequence ||
        Entry->Size < sizeof(HV_LOG_ENTRY) ||
        Entry->Size % HSECTOR_SIZE ||
        Entry->Size > LogSize - Offset ||
        !Entry->Length ||
        Entry->Length % HBLOCK_SIZE ||
        Entry->Length >= HCELL_TYPE_MASK ||
        Entry->DirtyPageCount > (Entry->Size - sizeof(HV_LOG_ENTRY)) / sizeof(HV_LOG_DIRTY_PAGE) ||
        Entry->Hash2 != HvpComputeLogHash(Entry, FIELD_OFFSET(HV_LOG_ENTRY, Hash2)) ||
        Entry->Hash1 != HvpComputeLogHash(Entry + 1, Entry->Size - sizeof(HV_LOG_ENTRY)))
    {
        return NULL;
    }

    Page = (PHV_LOG_DIRTY_PAGE)(Entry + 1);
    DataSize = Entry->Size - sizeof(HV_LOG_ENTRY) - Entry->DirtyPageCount * sizeof(HV_LOG_DIRTY_PAGE);
    for (i = 0; i < Entry->DirtyPageCount; i++)
    {
        if (!Page[i].Size ||
            Page[i].Size > DataSize ||
            Page[i].Offset > Entry->Length ||
            Page[i].Size > Entry->Length - Page[i].Offset)
        {
            return NULL;
        }
        DataSize -= Page[i].Size;
    }

    return Entry;
}

static
ULONG
HvpScanIncrementalLog(
    _In_ PVOID Log,
    _In_ ULONG LogSize,
    _Out_ PULONG EndSequence)
{
    PHV_LOG_ENTRY Entry;
    ULONG Offset, Sequence = ((PHBASE_BLOCK)Log)->Sequence1, Count = 0;

    for (Offset = HSECTOR_SIZE;
         (Entry = HvpGetLogEntry(Log, LogSize, Offset, Sequence)) != NULL;
         Offset += Entry->Size, Sequence++)
    {
        Count++;
    }

    *EndSequence = Sequence - 1;
    return Count;
}

BOOLEAN
CMAPI
HvpApplyIncrementalLog(
    _Inout_opt_ PHBASE_BLOCK BaseBlock,
    _In_ ULONG ImageLength,
    _In_ PVOID Log,
    _In_ ULONG LogSize,
    _In_ BOOLEAN Apply,
    _Inout_opt_ PRTL_BITMAP AppliedBlocks,
    _Out_ PULONG RequiredLength)
{
    PHBASE_BLOCK LogBaseBlock = Log;
    PHV_LOG_ENTRY Entry;
    PHV_LOG_DIRTY_PAGE Page;
    PUCHAR Data;
    BOOLEAN PrimaryValid;
    ULONG Offset, Sequence, Length, Applied, i;

    *RequiredLength = 0;
    if (LogSize < HSECTOR_SIZE + sizeof(HV_LOG_ENTRY) || !HvpIsLogBaseBlockValid(LogBaseBlock))
        return FALSE;

    ASSERT(BaseBlock || !Apply);
    PrimaryValid = BaseBlock &&
                   BaseBlock->Signature == HV_HBLOCK_SIGNATURE &&
                   HvpHiveHeaderChecksum(BaseBlock) == BaseBlock->CheckSum;
    if (PrimaryValid)
    {
        /* A log that ends before the primary's committed sequence is stale.
           One that starts earlier but reaches it (the previous cycle's log
           of an alternating pair) is a valid, redundant source. */
        if (BaseBlock->Sequence1 == BaseBlock->Sequence2 ||
            !HvpScanIncrementalLog(Log, LogSize, &Sequence) ||
            Sequence < BaseBlock->Sequence2)
        {
            return FALSE;
        }
    }

    Length = PrimaryValid ? BaseBlock->Length : LogBaseBlock->Length;
    if (Length >= HCELL_TYPE_MASK)
        return FALSE;

    Applied = 0;
    Sequence = LogBaseBlock->Sequence1;
    for (Offset = HSECTOR_SIZE;
         (Entry = HvpGetLogEntry(Log, LogSize, Offset, Sequence)) != NULL;
         Offset += Entry->Size, Sequence++)
    {
        if (Entry->Length > Length)
            Length = Entry->Length;
        if (Apply)
        {
            if (!Applied && !PrimaryValid)
                RtlCopyMemory(BaseBlock, LogBaseBlock, HSECTOR_SIZE);

            ASSERT(Entry->Length <= ImageLength);
            Page = (PHV_LOG_DIRTY_PAGE)(Entry + 1);
            Data = (PUCHAR)(Page + Entry->DirtyPageCount);
            for (i = 0; i < Entry->DirtyPageCount; i++)
            {
                RtlCopyMemory((PUCHAR)BaseBlock + HBLOCK_SIZE + Page[i].Offset, Data, Page[i].Size);
                if (AppliedBlocks)
                {
                    RtlSetBits(AppliedBlocks,
                               Page[i].Offset / HBLOCK_SIZE,
                               (Page[i].Offset % HBLOCK_SIZE + Page[i].Size + HBLOCK_SIZE - 1) / HBLOCK_SIZE);
                }
                Data += Page[i].Size;
            }

            BaseBlock->Length = Entry->Length;
            BaseBlock->Flags = (BaseBlock->Flags & ~HV_LOG_ENTRY_FLAGS) | (Entry->Flags & HV_LOG_ENTRY_FLAGS);
        }
        Applied++;
    }

    if (!Applied)
        return FALSE;

    if (Apply)
    {
        DPRINT1("Applied %lu hive log entries up to sequence 0x%lx\n", (unsigned long)Applied, (unsigned long)(Sequence - 1));
        BaseBlock->Type = HFILE_TYPE_PRIMARY;
        BaseBlock->Sequence1 = Sequence - 1;
        BaseBlock->Sequence2 = Sequence - 1;
        BaseBlock->CheckSum = HvpHiveHeaderChecksum(BaseBlock);
    }

    *RequiredLength = Length;
    return TRUE;
}

/**
 * @brief
 * Orders the usable incremental logs of a hive, newest first.
 *
 * @param[in] PrimaryBaseBlock
 * The primary base block, if it is readable.
 *
 * @param[in] Logs
 * The LOG1 and LOG2 images, NULL when absent.
 *
 * @param[in] LogSizes
 * The sizes of the two images in bytes.
 *
 * @param[out] Order
 * Receives the indexes (0 = LOG1, 1 = LOG2) of the logs that can bring
 * the primary forward, newest cycle first.
 *
 * @return
 * The number of usable logs. A log whose newest entry is torn is not
 * usable; the other log still covers the last committed state.
 */
ULONG
CMAPI
HvpSelectIncrementalLog(
    _In_opt_ PHBASE_BLOCK PrimaryBaseBlock,
    _In_ PVOID *Logs,
    _In_ PULONG LogSizes,
    _Out_ PULONG Order)
{
    ULONG Required, Count = 0, i, Tmp;

    for (i = 0; i < 2; i++)
    {
        if (Logs[i] && LogSizes[i] &&
            HvpApplyIncrementalLog(PrimaryBaseBlock, 0, Logs[i], LogSizes[i], FALSE, NULL, &Required))
        {
            Order[Count++] = i;
        }
    }

    if (Count == 2 &&
        ((PHBASE_BLOCK)Logs[Order[1]])->Sequence1 > ((PHBASE_BLOCK)Logs[Order[0]])->Sequence1)
    {
        Tmp = Order[0];
        Order[0] = Order[1];
        Order[1] = Tmp;
    }

    return Count;
}

/**
 * @brief
 * Clamps the hive header Length to the contiguous run of valid bins
 * actually present in an in-memory hive image.
 *
 * @param[in,out] BaseBlock
 * The base block of the hive image, followed by ImageLength bytes of
 * bin data. The Length field and checksum are updated in place when
 * a clamp was necessary.

 * @param[in] ImageLength
 * The size in bytes of the bin data that follows the base block.

 * @return
 * Returns the clamped Length (a multiple of HBLOCK_SIZE, possibly
 * equal to the incoming BaseBlock->Length when every bin is valid).
 *
 * @remarks
 * A growth write that the volume refuses (e.g. disk full) leaves the
 * primary header claiming bins the file does not hold. After the dirty
 * data of a log has been applied into an image, the region a log could
 * not cover stays zero-filled and fails bin validation at hive init.
 * Loading only the valid contiguous prefix keeps the hive bootable and
 * confines the loss to what was never written; the caller is expected
 * to mark the hive for self-heal.
 */
ULONG
CMAPI
HvpClampHiveToValidBins(
    _Inout_ PHBASE_BLOCK BaseBlock,
    _In_ ULONG ImageLength)
{
    ULONG Offset = 0;
    PUCHAR Image = (PUCHAR)BaseBlock;

    while (Offset + sizeof(HBIN) <= ImageLength)
    {
        PHBIN Bin = (PHBIN)(Image + HBLOCK_SIZE + Offset);

        if (Bin->Signature != HV_HBIN_SIGNATURE ||
            !Bin->Size ||
            (Bin->Size % HBLOCK_SIZE) != 0 ||
            Bin->Size > ImageLength - Offset ||
            Bin->FileOffset != Offset)
        {
            break;
        }

        Offset += Bin->Size;
    }

    if (Offset != BaseBlock->Length)
    {
        BaseBlock->Length = Offset;
        BaseBlock->CheckSum = HvpHiveHeaderChecksum(BaseBlock);
    }

    return Offset;
}

#if !defined(CMLIB_HOST) && !defined(_BLDR_)
ULONG
CMAPI
HvpQueryFileSize(
    _In_ PHHIVE Hive,
    _In_ ULONG FileType)
{
    HANDLE FileHandle = ((PCMHIVE)Hive)->FileHandles[FileType];
    FILE_STANDARD_INFORMATION FileStandard;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    if (!FileHandle)
        return 0;

    Status = ZwQueryInformationFile(FileHandle,
                                    &IoStatusBlock,
                                    &FileStandard,
                                    sizeof(FileStandard),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status) || FileStandard.EndOfFile.HighPart)
        return 0;

    return FileStandard.EndOfFile.LowPart;
}

static
NTSTATUS
HvpRecoverHiveFromLogImage(
    _In_ PHHIVE Hive,
    _In_opt_ PHBASE_BLOCK PrimaryBaseBlock,
    _In_ PVOID Log,
    _In_ ULONG LogSize,
    _Out_ PHBASE_BLOCK *HiveData,
    _Out_ PULONG HiveDataSize,
    _Out_ PRTL_BITMAP AppliedBlocks)
{
    PHBASE_BLOCK Image;
    PULONG BitmapBuffer;
    ULONG PrimarySize, Required, ImageSize, BitmapSize;
    ULONG FileOffset;

    *HiveData = NULL;
    *HiveDataSize = 0;

    if (!HvpApplyIncrementalLog(PrimaryBaseBlock, 0, Log, LogSize, FALSE, NULL, &Required))
        return STATUS_NOT_FOUND;

    ImageSize = HBLOCK_SIZE + Required;
    BitmapSize = ROUND_UP(Required / HBLOCK_SIZE, sizeof(ULONG) * 8) / 8;
    Image = Hive->Allocate(ImageSize, TRUE, TAG_CM);
    BitmapBuffer = Hive->Allocate(BitmapSize, TRUE, TAG_CM);
    if (!Image || !BitmapBuffer)
    {
        if (Image)
            Hive->Free(Image, ImageSize);
        if (BitmapBuffer)
            Hive->Free(BitmapBuffer, BitmapSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Image, ImageSize);
    RtlInitializeBitMap(AppliedBlocks, BitmapBuffer, BitmapSize * 8);
    RtlClearAllBits(AppliedBlocks);

    if (PrimaryBaseBlock)
        RtlCopyMemory(Image, PrimaryBaseBlock, HBLOCK_SIZE);

    PrimarySize = ROUND_DOWN(HvpQueryFileSize(Hive, HFILE_TYPE_PRIMARY), HSECTOR_SIZE);
    PrimarySize = (PrimarySize > HBLOCK_SIZE) ? min(PrimarySize - HBLOCK_SIZE, Required) : 0;
    FileOffset = HBLOCK_SIZE;
    if (PrimarySize &&
        !Hive->FileRead(Hive, HFILE_TYPE_PRIMARY, &FileOffset, (PUCHAR)Image + HBLOCK_SIZE, PrimarySize))
    {
        DPRINT1("Failed to read the primary hive data for log recovery\n");
        Hive->Free(BitmapBuffer, BitmapSize);
        Hive->Free(Image, ImageSize);
        return STATUS_REGISTRY_IO_FAILED;
    }

    HvpApplyIncrementalLog(Image, Required, Log, LogSize, TRUE, AppliedBlocks, &Required);

    /* A refused growth write leaves the header claiming bins neither the
       file nor the log holds; the untouched tail of the image stays
       zero-filled and would fail bin validation. Clamp to what is valid
       so the hive loads, and flag the loss for self-heal. */
    if (HvpClampHiveToValidBins(Image, Required) != Required)
        Image->BootType |= HBOOT_TYPE_SELF_HEAL;

    *HiveData = Image;
    *HiveDataSize = ImageSize;
    return STATUS_SUCCESS;
}

NTSTATUS
CMAPI
HvpRecoverHiveFromLog(
    _In_ PHHIVE Hive,
    _In_opt_ PHBASE_BLOCK PrimaryBaseBlock,
    _Out_ PHBASE_BLOCK *HiveData,
    _Out_ PULONG HiveDataSize,
    _Out_ PRTL_BITMAP AppliedBlocks,
    _Out_ PULONG LogType)
{
    static const ULONG Types[2] = { HFILE_TYPE_LOG, HFILE_TYPE_LOG2 };
    PVOID Logs[2] = { NULL, NULL };
    ULONG Sizes[2] = { 0, 0 }, Allocated[2] = { 0, 0 }, Order[2];
    ULONG FileOffset, Count, i;
    NTSTATUS Status;

    *HiveData = NULL;
    *HiveDataSize = 0;
    *LogType = HFILE_TYPE_LOG;

    /* LOG2 is absent on hives written before alternation: that is just a
       single log to choose from */
    for (i = 0; i < 2; i++)
    {
        Sizes[i] = ROUND_DOWN(HvpQueryFileSize(Hive, Types[i]), HSECTOR_SIZE);
        if (Sizes[i] < HSECTOR_SIZE + sizeof(HV_LOG_ENTRY))
        {
            Sizes[i] = 0;
            continue;
        }

        Logs[i] = Hive->Allocate(Sizes[i], TRUE, TAG_CM);
        if (!Logs[i])
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        Allocated[i] = Sizes[i];

        FileOffset = 0;
        if (!Hive->FileRead(Hive, Types[i], &FileOffset, Logs[i], Sizes[i]))
            Sizes[i] = 0;
    }

    Count = HvpSelectIncrementalLog(PrimaryBaseBlock, Logs, Sizes, Order);
    Status = STATUS_NOT_FOUND;
    for (i = 0; i < Count; i++)
    {
        Status = HvpRecoverHiveFromLogImage(Hive,
                                            PrimaryBaseBlock,
                                            Logs[Order[i]],
                                            Sizes[Order[i]],
                                            HiveData,
                                            HiveDataSize,
                                            AppliedBlocks);
        if (Status != STATUS_NOT_FOUND)
        {
            *LogType = Types[Order[i]];
            break;
        }
    }

Exit:
    for (i = 0; i < 2; i++)
    {
        if (Logs[i])
            Hive->Free(Logs[i], Allocated[i]);
    }
    return Status;
}
#endif

/* EOF */
