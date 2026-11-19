/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Non-volatile memory token, write, fill, flush and drain routines
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define RTL_NV_MEMORY_TOKEN_TAG 'TNVM'

typedef struct _RTL_NV_MEMORY_TOKEN
{
    ULONG Tag;
    PVOID NvBuffer;
    SIZE_T Size;
} RTL_NV_MEMORY_TOKEN, *PRTL_NV_MEMORY_TOKEN;

/* PRIVATE FUNCTIONS *********************************************************/

static
BOOLEAN
RtlpIsValidNvToken(
    _In_ PVOID NvToken)
{
    PRTL_NV_MEMORY_TOKEN Token = NvToken;

    if (NvToken == (PVOID)FLUSH_NV_MEMORY_DEFAULT_TOKEN)
        return TRUE;
    return (Token != NULL) && (Token->Tag == RTL_NV_MEMORY_TOKEN_TAG);
}

static
VOID
RtlpFlushNvRange(
    _In_ PVOID Base,
    _In_ SIZE_T Length)
{
    PUCHAR Current = Base;
    ULONG Chunk;

    while (Length != 0)
    {
        Chunk = (ULONG)min(Length, (SIZE_T)0x80000000);
        KeInvalidateRangeAllCaches(Current, Chunk);
        Current += Chunk;
        Length -= Chunk;
    }
}

static
VOID
RtlpDrainNvFlush(VOID)
{
#if defined(_M_AMD64)
    _mm_sfence();
#else
    KeMemoryBarrier();
#endif
}

static
VOID
RtlpCopyNonTemporal(
    _Out_writes_bytes_all_(Length) PUCHAR Destination,
    _In_reads_bytes_(Length) const UCHAR *Source,
    _In_ SIZE_T Length)
{
#if defined(_M_AMD64)
    ULONG64 Value;

    /* MOVNTI needs an aligned quadword */
    while ((Length != 0) && (((ULONG_PTR)Destination & 7) != 0))
    {
        *Destination++ = *Source++;
        Length--;
    }
    while (Length >= 8)
    {
        RtlCopyMemory(&Value, Source, sizeof(Value));
        _mm_stream_si64((long long *)Destination, (long long)Value);
        Destination += 8;
        Source += 8;
        Length -= 8;
    }
#endif
    RtlCopyMemory(Destination, Source, Length);
}

static
NTSTATUS
RtlpStoreNonVolatileMemory(
    _In_ PVOID NvToken,
    _Out_writes_bytes_(Size) PVOID NvDestination,
    _In_reads_bytes_opt_(Size) const VOID *Source,
    _In_ SIZE_T Size,
    _In_ UCHAR Value,
    _In_ ULONG Flags)
{
    /* The write and fill flags have the same values */
    C_ASSERT(WRITE_NV_MEMORY_FLAG_PERSIST == FILL_NV_MEMORY_FLAG_PERSIST);
    C_ASSERT(WRITE_NV_MEMORY_FLAG_NO_DRAIN == FILL_NV_MEMORY_FLAG_NO_DRAIN);

    if (!RtlpIsValidNvToken(NvToken) ||
        (Flags & ~(WRITE_NV_MEMORY_FLAG_PERSIST | WRITE_NV_MEMORY_FLAG_NO_DRAIN)) ||
        ((Size != 0) && (NvDestination == NULL)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (Size == 0)
        return STATUS_SUCCESS;

    if (Flags & WRITE_NV_MEMORY_FLAG_NON_TEMPORAL)
    {
        if (Source != NULL)
            RtlpCopyNonTemporal(NvDestination, Source, Size);
        else
            RtlFillMemoryNonTemporal(NvDestination, Size, Value);
    }
    else
    {
        if (Source != NULL)
            RtlCopyMemory(NvDestination, Source, Size);
        else
            RtlFillMemory(NvDestination, Size, Value);
    }

    /* Bytes outside the aligned non-temporal stores went through the cache, so flush them too */
    if (Flags & WRITE_NV_MEMORY_FLAG_PERSIST)
    {
        RtlpFlushNvRange(NvDestination, Size);

        /* NO_DRAIN applies only to a plain flush request */
        if ((Flags & WRITE_NV_MEMORY_FLAG_NON_TEMPORAL) || !(Flags & WRITE_NV_MEMORY_FLAG_NO_DRAIN))
            RtlpDrainNvFlush();
    }

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS **********************************************************/

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

    /* Nonpaged: the write and flush routines check the token up to DISPATCH_LEVEL */
    Token = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Token), RTL_NV_MEMORY_TOKEN_TAG);
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

    if ((Token == NULL) || (NvToken == (PVOID)FLUSH_NV_MEMORY_DEFAULT_TOKEN) ||
        (Token->Tag != RTL_NV_MEMORY_TOKEN_TAG))
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
    if (!RtlpIsValidNvToken(NvToken) ||
        (NvBuffer == NULL) || (Size == 0) ||
        (Flags & ~FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN))
        return STATUS_INVALID_PARAMETER;

    RtlpFlushNvRange(NvBuffer, Size);
    if (!(Flags & FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN))
        RtlpDrainNvFlush();

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlFlushNonVolatileMemoryRanges(
    _In_ PVOID NvToken,
    _In_reads_(NumRanges) PNV_MEMORY_RANGE NvRanges,
    _In_ SIZE_T NumRanges,
    _In_ ULONG Flags)
{
    SIZE_T Index;

    if (!RtlpIsValidNvToken(NvToken) ||
        ((NumRanges != 0) && (NvRanges == NULL)) ||
        (Flags & ~FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN))
        return STATUS_INVALID_PARAMETER;

    for (Index = 0; Index < NumRanges; Index++)
    {
        if ((NvRanges[Index].BaseAddress == NULL) && (NvRanges[Index].Length != 0))
            return STATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < NumRanges; Index++)
        RtlpFlushNvRange(NvRanges[Index].BaseAddress, NvRanges[Index].Length);

    if (!(Flags & FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN))
        RtlpDrainNvFlush();

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlDrainNonVolatileFlush(
    _In_ PVOID NvToken)
{
    if (!RtlpIsValidNvToken(NvToken))
        return STATUS_INVALID_PARAMETER;

    RtlpDrainNvFlush();
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlWriteNonVolatileMemory(
    _In_ PVOID NvToken,
    _Out_writes_bytes_(Size) VOID UNALIGNED *NvDestination,
    _In_reads_bytes_(Size) CONST VOID UNALIGNED *Source,
    _In_ SIZE_T Size,
    _In_ ULONG Flags)
{
    if ((Size != 0) && (Source == NULL))
        return STATUS_INVALID_PARAMETER;

    return RtlpStoreNonVolatileMemory(NvToken, (PVOID)NvDestination, Source, Size, 0, Flags);
}

NTSTATUS
NTAPI
RtlFillNonVolatileMemory(
    _In_ PVOID NvToken,
    _Out_writes_bytes_(Size) VOID UNALIGNED *NvDestination,
    _In_ SIZE_T Size,
    _In_ CONST UCHAR Value,
    _In_ ULONG Flags)
{
    return RtlpStoreNonVolatileMemory(NvToken, (PVOID)NvDestination, NULL, Size, Value, Flags);
}

/* EOF */
