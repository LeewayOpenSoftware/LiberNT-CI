/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Validation of the mount-point reparse buffers used for name grafting
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>

#define IOP_MOUNT_POINT_FIELDS_LENGTH \
    (FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) - REPARSE_DATA_BUFFER_HEADER_SIZE)

static
BOOLEAN
IopIsNameInPathBuffer(
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ ULONG PathLength)
{
    return !(Offset & 1) && !(Length & 1) && Offset <= PathLength && Length <= PathLength - Offset;
}

NTSTATUS
NTAPI
IopValidateMountPointBuffer(
    _In_reads_bytes_(BufferLength) PREPARSE_DATA_BUFFER Buffer,
    _In_ ULONG BufferLength)
{
    ULONG DataLength, PathLength;
    ULONG SubstituteOffset, SubstituteLength, PrintOffset, PrintLength;
    PCWSTR Substitute;

    if (Buffer == NULL || BufferLength < FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer))
        return STATUS_IO_REPARSE_DATA_INVALID;

    if (Buffer->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT)
        return STATUS_IO_REPARSE_DATA_INVALID;

    DataLength = Buffer->ReparseDataLength;
    if (DataLength < IOP_MOUNT_POINT_FIELDS_LENGTH ||
        DataLength > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - REPARSE_DATA_BUFFER_HEADER_SIZE ||
        DataLength > BufferLength - REPARSE_DATA_BUFFER_HEADER_SIZE)
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    PathLength = DataLength - IOP_MOUNT_POINT_FIELDS_LENGTH;
    SubstituteOffset = Buffer->MountPointReparseBuffer.SubstituteNameOffset;
    SubstituteLength = Buffer->MountPointReparseBuffer.SubstituteNameLength;
    PrintOffset = Buffer->MountPointReparseBuffer.PrintNameOffset;
    PrintLength = Buffer->MountPointReparseBuffer.PrintNameLength;

    if (SubstituteLength < sizeof(WCHAR) ||
        !IopIsNameInPathBuffer(SubstituteOffset, SubstituteLength, PathLength) ||
        !IopIsNameInPathBuffer(PrintOffset, PrintLength, PathLength))
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    if (PrintLength != 0 &&
        SubstituteOffset < PrintOffset + PrintLength &&
        PrintOffset < SubstituteOffset + SubstituteLength)
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    Substitute = (PCWSTR)((PUCHAR)Buffer->MountPointReparseBuffer.PathBuffer + SubstituteOffset);
    if (Substitute[0] != OBJ_NAME_PATH_SEPARATOR)
        return STATUS_IO_REPARSE_DATA_INVALID;

    return STATUS_SUCCESS;
}
