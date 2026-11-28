#include "precomp.h"

/*
 * @unimplemented
 */
VOID
NTAPI
TdiBuildNetbiosAddress (
	IN	PUCHAR			NetbiosName,
	IN	BOOLEAN			IsGroupName,
	IN OUT	PTA_NETBIOS_ADDRESS	NetworkName
        )
{
}


/*
 * @unimplemented
 */
NTSTATUS
NTAPI
TdiBuildNetbiosAddressEa (
	IN	PUCHAR	Buffer,
	IN	BOOLEAN	GroupName,
	IN	PUCHAR	NetbiosName
	)
{
	return STATUS_NOT_IMPLEMENTED;
}


/*
 * Copies from a buffer into the buffers that a chain of MDLs describes.
 * Returns STATUS_BUFFER_OVERFLOW when the chain is too short for the data;
 * BytesCopied is then what did fit.
 *
 * @implemented
 */
NTSTATUS
NTAPI
TdiCopyBufferToMdl (
	IN	PVOID	SourceBuffer,
	IN	ULONG	SourceOffset,
	IN	ULONG	SourceBytesToCopy,
	IN	PMDL	DestinationMdlChain,
	IN	ULONG	DestinationOffset,
	IN	PULONG	BytesCopied
	)
{
	PUCHAR Source = (PUCHAR)SourceBuffer + SourceOffset;
	PMDL Mdl = DestinationMdlChain;
	ULONG Remaining = SourceBytesToCopy;
	ULONG MdlLength, Chunk;
	PUCHAR Mapped;

	*BytesCopied = 0;

	/* Find the MDL in which the destination offset lies */
	while (Mdl != NULL && DestinationOffset >= MmGetMdlByteCount(Mdl))
	{
		DestinationOffset -= MmGetMdlByteCount(Mdl);
		Mdl = Mdl->Next;
	}

	while (Mdl != NULL && Remaining != 0)
	{
		MdlLength = MmGetMdlByteCount(Mdl) - DestinationOffset;
		if (MdlLength != 0)
		{
			Mapped = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
			if (Mapped == NULL)
				return STATUS_INSUFFICIENT_RESOURCES;

			Chunk = min(MdlLength, Remaining);
			RtlCopyMemory(Mapped + DestinationOffset, Source, Chunk);
			Source += Chunk;
			Remaining -= Chunk;
			*BytesCopied += Chunk;
		}

		DestinationOffset = 0;
		Mdl = Mdl->Next;
	}

	return (Remaining != 0) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}


/*
 * Copies from the buffers that a chain of MDLs describes into a buffer.
 * Returns STATUS_BUFFER_OVERFLOW when the buffer is too small for the data
 * of the chain; BytesCopied is then what did fit.
 *
 * @implemented
 */
NTSTATUS
NTAPI
TdiCopyMdlToBuffer (
	IN	PMDL	SourceMdlChain,
	IN	ULONG	SourceOffset,
	IN	PVOID	DestinationBuffer,
	IN	ULONG	DestinationOffset,
	IN	ULONG	DestinationBufferSize,
	OUT	PULONG	BytesCopied
	)
{
	PUCHAR Destination = (PUCHAR)DestinationBuffer + DestinationOffset;
	PMDL Mdl = SourceMdlChain;
	ULONG Room = (DestinationOffset < DestinationBufferSize) ?
	             (DestinationBufferSize - DestinationOffset) : 0;
	ULONG MdlLength, Chunk;
	PUCHAR Mapped;

	*BytesCopied = 0;

	/* Find the MDL in which the source offset lies */
	while (Mdl != NULL && SourceOffset >= MmGetMdlByteCount(Mdl))
	{
		SourceOffset -= MmGetMdlByteCount(Mdl);
		Mdl = Mdl->Next;
	}

	while (Mdl != NULL)
	{
		MdlLength = MmGetMdlByteCount(Mdl) - SourceOffset;
		if (MdlLength != 0)
		{
			/* There is more data and no more room */
			if (Room == 0)
				return STATUS_BUFFER_OVERFLOW;

			Mapped = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
			if (Mapped == NULL)
				return STATUS_INSUFFICIENT_RESOURCES;

			Chunk = min(MdlLength, Room);
			RtlCopyMemory(Destination, Mapped + SourceOffset, Chunk);
			Destination += Chunk;
			Room -= Chunk;
			*BytesCopied += Chunk;

			if (Chunk < MdlLength)
				return STATUS_BUFFER_OVERFLOW;
		}

		SourceOffset = 0;
		Mdl = Mdl->Next;
	}

	return STATUS_SUCCESS;
}


/*
 * @unimplemented
 */
VOID
NTAPI
TdiInitialize ()
{
}


/*
 * @unimplemented
 */
NTSTATUS
NTAPI
TdiMapUserRequest (
	IN	PDEVICE_OBJECT		DeviceObject,
	IN	PIRP			Irp,
	IN	PIO_STACK_LOCATION	IrpSp
	)
{
	return STATUS_NOT_IMPLEMENTED;
}


/*
 * @unimplemented
 */
VOID
NTAPI
TdiOpenNetbiosAddress (
	ULONG   Unknown0,
	ULONG   Unknown1,
	ULONG   Unknown2,
	ULONG   Unknown3
	)
{
}


/*
 * @unimplemented
 */
VOID
NTAPI
TdiReturnChainedReceives (
	IN	PVOID	* TsduDescriptors,
	IN	ULONG	NumberOfTsdus
        )
{
}

/* EOF */
