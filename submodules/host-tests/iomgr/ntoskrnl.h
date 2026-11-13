/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host stand-in for ntoskrnl.h, enough for ntoskrnl/io/iomgr/reparse.c
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t NTSTATUS;
typedef uint8_t UCHAR, *PUCHAR, BOOLEAN;
typedef uint16_t USHORT, WCHAR;
typedef const WCHAR *PCWSTR;
typedef uint32_t ULONG;

#define NTAPI
#define _In_
#define _In_reads_bytes_(n)

#define TRUE 1
#define FALSE 0
#define FIELD_OFFSET(Type, Field) ((ULONG)offsetof(Type, Field))
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_IO_REPARSE_DATA_INVALID ((NTSTATUS)0xC0000278)
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003u
#define IO_REPARSE_TAG_SYMLINK 0xA000000Cu
#define OBJ_NAME_PATH_SEPARATOR ((WCHAR)0x005C)
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)

typedef struct _REPARSE_DATA_BUFFER
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union
    {
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct
        {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_SIZE FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

_Static_assert(offsetof(REPARSE_DATA_BUFFER, GenericReparseBuffer) == 8, "REPARSE_DATA_BUFFER header layout");
_Static_assert(offsetof(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) == 16, "mount point layout");
_Static_assert(offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer) == 20, "symbolic link layout");

NTSTATUS
NTAPI
IopValidateMountPointBuffer(
    _In_reads_bytes_(BufferLength) PREPARSE_DATA_BUFFER Buffer,
    _In_ ULONG BufferLength);
