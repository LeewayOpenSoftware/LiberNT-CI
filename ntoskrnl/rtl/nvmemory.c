/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Non-volatile memory token and flush routines
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define RTL_NV_MEMORY_TOKEN_TAG 'TVN'

typedef struct _RTL_NV_MEMORY_TOKEN
{
    ULONG Tag;
    PVOID NvBuffer;
    SIZE_T Size;
} RTL_NV_MEMORY_TOKEN, *PRTL_NV_MEMORY_TOKEN;

NTSTATUS
NTAPI
RtlGetNonVolatileToken(IN PVOID NvBuffer,
                       IN SIZE_T Size,
                       OUT PVOID *NvToken)
{
    PRTL_NV_MEMORY_TOKEN Token;

    PAGED_CODE();

    if ((NvBuffer == NULL) || (Size == 0) || (NvToken == NULL))
        return STATUS_INVALID_PARAMETER;

    Token = ExAllocatePoolZero(PagedPool, sizeof(*Token), RTL_NV_MEMORY_TOKEN_TAG);
    if (Token == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Token->Tag = RTL_NV_MEMORY_TOKEN_TAG;
    Token->NvBuffer = NvBuffer;
    Token->Size = Size;

    *NvToken = Token;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlFreeNonVolatileToken(IN PVOID NvToken)
{
    PRTL_NV_MEMORY_TOKEN Token = NvToken;

    PAGED_CODE();

    if ((Token == NULL) || (Token->Tag != RTL_NV_MEMORY_TOKEN_TAG))
        return STATUS_INVALID_PARAMETER;

    Token->Tag = 0;
    ExFreePoolWithTag(Token, RTL_NV_MEMORY_TOKEN_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlFlushNonVolatileMemory(IN PVOID NvToken,
                          IN PVOID NvBuffer,
                          IN SIZE_T Size,
                          IN ULONG Flags)
{
    PRTL_NV_MEMORY_TOKEN Token = NvToken;

    PAGED_CODE();

    if ((Token == NULL) || (Token->Tag != RTL_NV_MEMORY_TOKEN_TAG) ||
        (NvBuffer == NULL) || (Size == 0) ||
        (Flags & ~FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN))
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}
