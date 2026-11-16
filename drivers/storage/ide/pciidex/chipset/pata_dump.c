/*
 * PROJECT:     ReactOS ATA Bus Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PATA crash-dump storage interface (polled, interrupt-free)
 * COPYRIGHT:   Copyright 2026 Leeway Development Works
 */

/* INCLUDES *******************************************************************/

#include "pciidex.h"
#include <reactos/drivers/dumpstor.h>
#include <reactos/drivers/dumpscsi.h>

/* TYPES **********************************************************************/

typedef struct _PATA_DUMP_CONTEXT
{
    PCHANNEL_DATA_PATA ChanData;
    UCHAR DeviceSelect;
    BOOLEAN Lba48;
    ULONG SectorSize;
} PATA_DUMP_CONTEXT, *PPATA_DUMP_CONTEXT;

/* PRIVATE FUNCTIONS **********************************************************/

static UCHAR
PataDumpReadStatus(
    _In_ PCHANNEL_DATA_PATA ChanData)
{
    return ATA_READ(ChanData->Regs.AlternateStatus, ChanData, MRES_CTRL);
}

/* Wait until no bit of Mask is set in the alternate status. */
static BOOLEAN
PataDumpWaitClear(
    _In_ PCHANNEL_DATA_PATA ChanData,
    _In_ UCHAR Mask)
{
    ULONG i;

    for (i = 0; i < ROS_DUMP_POLL_ITERATIONS; i++)
    {
        if ((PataDumpReadStatus(ChanData) & Mask) == 0)
            return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

/* Wait until every bit of Mask is set; fails early on a non-busy error status. */
static BOOLEAN
PataDumpWaitSet(
    _In_ PCHANNEL_DATA_PATA ChanData,
    _In_ UCHAR Mask)
{
    ULONG i;
    UCHAR Status;

    for (i = 0; i < ROS_DUMP_POLL_ITERATIONS; i++)
    {
        Status = PataDumpReadStatus(ChanData);
        if (!(Status & IDE_STATUS_BUSY) && (Status & IDE_STATUS_ERROR))
            return FALSE;
        if ((Status & Mask) == Mask)
            return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static NTSTATUS
PataDumpSelectDevice(
    _In_ PPATA_DUMP_CONTEXT DumpContext)
{
    PCHANNEL_DATA_PATA ChanData = DumpContext->ChanData;

    ATA_WRITE(ChanData->Regs.Device,
              (UCHAR)(DumpContext->DeviceSelect | IDE_LBA_MODE),
              ChanData,
              MRES_TF);
    KeStallExecutionProcessor(1);

    if (!PataDumpWaitClear(ChanData, IDE_STATUS_BUSY))
        return STATUS_IO_TIMEOUT;

    if (PataDumpReadStatus(ChanData) & IDE_STATUS_ERROR)
        return STATUS_IO_DEVICE_ERROR;

    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
PataDumpPrepare(
    _In_ PVOID Context)
{
    return PataDumpSelectDevice((PPATA_DUMP_CONTEXT)Context);
}

static NTSTATUS NTAPI
PataDumpWrite(
    _In_ PVOID Context,
    _In_ ULONG64 DiskByteOffset,
    _In_ PHYSICAL_ADDRESS Buffer,
    _In_ ULONG Length)
{
    PPATA_DUMP_CONTEXT DumpContext = Context;
    PCHANNEL_DATA_PATA ChanData = DumpContext->ChanData;
    ULONG64 BlockAddress;
    ULONG Remaining, ChunkSectors, ThisLength, Done, InPage;
    PHYSICAL_ADDRESS Pa;
    UCHAR DeviceValue;

    if ((Length == 0) || (Length > PAGE_SIZE) ||
        (((DiskByteOffset | Length) % DumpContext->SectorSize) != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    BlockAddress = DiskByteOffset / DumpContext->SectorSize;
    Pa = Buffer;
    Remaining = Length;

    while (Remaining > 0)
    {
        ChunkSectors = (ULONG)min((ULONG64)(Remaining / DumpContext->SectorSize), 256);

        if (!NT_SUCCESS(PataDumpSelectDevice(DumpContext)))
            return STATUS_IO_TIMEOUT;

        /* Program the task file; 48-bit addressing writes the previous
           registers before the current ones. */
        if (DumpContext->Lba48)
        {
            ATA_WRITE(ChanData->Regs.SectorCount, (UCHAR)(ChunkSectors >> 8), ChanData, MRES_TF);
            ATA_WRITE(ChanData->Regs.LbaLow, (UCHAR)(BlockAddress >> 24), ChanData, MRES_TF);
            ATA_WRITE(ChanData->Regs.LbaMid, (UCHAR)(BlockAddress >> 32), ChanData, MRES_TF);
            ATA_WRITE(ChanData->Regs.LbaHigh, (UCHAR)(BlockAddress >> 40), ChanData, MRES_TF);
        }
        ATA_WRITE(ChanData->Regs.SectorCount, (UCHAR)ChunkSectors, ChanData, MRES_TF);
        ATA_WRITE(ChanData->Regs.LbaLow, (UCHAR)BlockAddress, ChanData, MRES_TF);
        ATA_WRITE(ChanData->Regs.LbaMid, (UCHAR)(BlockAddress >> 8), ChanData, MRES_TF);
        ATA_WRITE(ChanData->Regs.LbaHigh, (UCHAR)(BlockAddress >> 16), ChanData, MRES_TF);

        if (DumpContext->Lba48)
            DeviceValue = (UCHAR)((DumpContext->DeviceSelect | IDE_LBA_MODE) & 0xF0);
        else
            DeviceValue = (UCHAR)((DumpContext->DeviceSelect | IDE_LBA_MODE) | ((BlockAddress >> 24) & 0x0F));
        ATA_WRITE(ChanData->Regs.Device, DeviceValue, ChanData, MRES_TF);
        KeStallExecutionProcessor(1);

        ATA_WRITE(ChanData->Regs.Command,
                  DumpContext->Lba48 ? IDE_COMMAND_WRITE_EXT : IDE_COMMAND_WRITE,
                  ChanData,
                  MRES_TF);

        ThisLength = ChunkSectors * DumpContext->SectorSize;
        for (Done = 0; Done < ThisLength; )
        {
            if (!PataDumpWaitSet(ChanData, IDE_STATUS_DRQ))
                return STATUS_IO_TIMEOUT;

            InPage = PAGE_SIZE - (Pa.LowPart & (PAGE_SIZE - 1));
            if (InPage > (ThisLength - Done))
                InPage = ThisLength - Done;

            AtaWriteBlock16((PUSHORT)ChanData->Regs.Data,
                            (PUSHORT)MmGetVirtualForPhysical(Pa),
                            InPage / sizeof(USHORT),
                            ChanData->ChanInfo & CHANNEL_FLAG_MRES_TF);

            Done += InPage;
            Pa.QuadPart += InPage;
        }

        if (!PataDumpWaitClear(ChanData, IDE_STATUS_BUSY))
            return STATUS_IO_TIMEOUT;

        if (PataDumpReadStatus(ChanData) & IDE_STATUS_ERROR)
            return STATUS_IO_DEVICE_ERROR;

        BlockAddress += ChunkSectors;
        Remaining -= ThisLength;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
PataDumpFlush(
    _In_ PVOID Context)
{
    PPATA_DUMP_CONTEXT DumpContext = Context;
    PCHANNEL_DATA_PATA ChanData = DumpContext->ChanData;
    NTSTATUS Status;

    Status = PataDumpSelectDevice(DumpContext);
    if (!NT_SUCCESS(Status))
        return Status;

    ATA_WRITE(ChanData->Regs.Command,
              DumpContext->Lba48 ? IDE_COMMAND_FLUSH_CACHE_EXT : IDE_COMMAND_FLUSH_CACHE,
              ChanData,
              MRES_TF);

    if (!PataDumpWaitClear(ChanData, IDE_STATUS_BUSY))
        return STATUS_IO_TIMEOUT;

    if (PataDumpReadStatus(ChanData) & IDE_STATUS_ERROR)
        return STATUS_IO_DEVICE_ERROR;

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

NTSTATUS
PataChannelGetDumpInterface(
    _In_ PVOID ChannelContext,
    _In_ UCHAR DeviceSelect,
    _In_ BOOLEAN Lba48,
    _In_ ULONG SectorSize,
    _Inout_ PROS_STORAGE_DUMP_INTERFACE Interface)
{
    PCHANNEL_DATA_PATA ChanData = ChannelContext;
    PPATA_DUMP_CONTEXT DumpContext;
    ULONG DeviceNumber = (DeviceSelect >> 4) & 0x1;

    if (!RosDumpIsValidSectorSize(SectorSize))
        return STATUS_INVALID_PARAMETER;

    DumpContext = ChanData->DumpContexts[DeviceNumber];
    if (DumpContext == NULL)
    {
        DumpContext = ExAllocatePoolZero(NonPagedPool, sizeof(*DumpContext), TAG_PCIIDEX);
        if (DumpContext == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        DumpContext->ChanData = ChanData;
        DumpContext->SectorSize = SectorSize;
        ChanData->DumpContexts[DeviceNumber] = DumpContext;
    }
    else if (DumpContext->SectorSize != SectorSize)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    DumpContext->DeviceSelect = DeviceSelect;
    DumpContext->Lba48 = Lba48;

    Interface->Version = ROS_STORAGE_DUMP_INTERFACE_VERSION;
    Interface->Size = sizeof(*Interface);
    Interface->Context = DumpContext;
    Interface->BytesPerSector = SectorSize;
    Interface->MaximumTransferLength = PAGE_SIZE;
    Interface->Prepare = PataDumpPrepare;
    Interface->WriteRoutine = PataDumpWrite;
    Interface->Flush = PataDumpFlush;
    return STATUS_SUCCESS;
}
