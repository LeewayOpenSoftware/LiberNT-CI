/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Correlation vectors, non-temporal fill and Cloud Files placeholder test
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#ifndef IO_REPARSE_TAG_CLOUD
#define IO_REPARSE_TAG_CLOUD      (0x9000001AL)
#endif
#ifndef IO_REPARSE_TAG_CLOUD_MASK
#define IO_REPARSE_TAG_CLOUD_MASK (0x0000F000L)
#endif

/* CORRELATION VECTORS *******************************************************/

static const CHAR RtlpCvBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static
ULONG
RtlpCvPrefixLength(
    _In_ CHAR Version)
{
    if (Version == RTL_CORRELATION_VECTOR_VERSION_1)
        return RTL_CORRELATION_VECTOR_V1_PREFIX_LENGTH;
    if (Version == RTL_CORRELATION_VECTOR_VERSION_2)
        return RTL_CORRELATION_VECTOR_V2_PREFIX_LENGTH;
    return 0;
}

/* The longest vector, without its terminator */
static
ULONG
RtlpCvMaxLength(
    _In_ CHAR Version)
{
    if (Version == RTL_CORRELATION_VECTOR_VERSION_1)
        return RTL_CORRELATION_VECTOR_V1_LENGTH - 1;
    return RTL_CORRELATION_VECTOR_V2_LENGTH - 1;
}

static
BOOLEAN
RtlpCvIsBase64(
    _In_ CHAR Char)
{
    return (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') ||
           (Char >= '0' && Char <= '9') || Char == '+' || Char == '/';
}

/* Returns the vector length, or 0 if it does not follow the specification */
static
ULONG
RtlpCvValidate(
    _In_ PCORRELATION_VECTOR Vector)
{
    ULONG Prefix = RtlpCvPrefixLength(Vector->Version);
    ULONG Length, Index, Digits;
    CHAR Last;

    if (Prefix == 0)
        return 0;

    for (Length = 0; Length < RTL_CORRELATION_VECTOR_STRING_LENGTH && Vector->Vector[Length] != ANSI_NULL; Length++);
    if (Length == RTL_CORRELATION_VECTOR_STRING_LENGTH || Length > RtlpCvMaxLength(Vector->Version) ||
        Length < Prefix + 2)
        return 0;

    for (Index = 0; Index < Prefix; Index++)
    {
        if (!RtlpCvIsBase64(Vector->Vector[Index]))
            return 0;
    }

    /* 22 base64 characters carry 132 bits: the 4 bits past the 16 bytes must be zero */
    Last = Vector->Vector[Prefix - 1];
    if (Vector->Version == RTL_CORRELATION_VECTOR_VERSION_2 &&
        Last != 'A' && Last != 'Q' && Last != 'g' && Last != 'w')
        return 0;

    /* The rest is (\.\d+)+ */
    for (Index = Prefix; Index < Length;)
    {
        if (Vector->Vector[Index++] != '.')
            return 0;
        for (Digits = 0; Index < Length && Vector->Vector[Index] >= '0' && Vector->Vector[Index] <= '9'; Index++)
            Digits++;
        if (Digits == 0)
            return 0;
    }

    return Length;
}

NTSTATUS
NTAPI
RtlInitializeCorrelationVector(
    _Out_ PCORRELATION_VECTOR CorrelationVector,
    _In_ int Version,
    _In_ const GUID *Guid)
{
    const UCHAR *Bytes = (const UCHAR *)Guid;
    ULONG Prefix, Out = 0, In = 0, Bits = 0, Accumulator = 0;

    if (CorrelationVector == NULL || Guid == NULL)
        return STATUS_INVALID_PARAMETER;

    Prefix = RtlpCvPrefixLength((CHAR)Version);
    if (Prefix == 0)
        return STATUS_INVALID_PARAMETER;

    /* Base64 of the GUID bytes without padding: 16 characters hold 12 bytes, 22 hold all 16 */
    while (Out < Prefix)
    {
        if (Bits < 6)
        {
            Accumulator = (Accumulator << 8) | (In < sizeof(GUID) ? Bytes[In] : 0);
            In++;
            Bits += 8;
        }
        Bits -= 6;
        CorrelationVector->Vector[Out++] = RtlpCvBase64[(Accumulator >> Bits) & 0x3F];
        Accumulator &= (1UL << Bits) - 1;
    }

    CorrelationVector->Vector[Out++] = '.';
    CorrelationVector->Vector[Out++] = '0';
    CorrelationVector->Vector[Out] = ANSI_NULL;
    CorrelationVector->Version = (CHAR)Version;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlIncrementCorrelationVector(
    _Inout_ PCORRELATION_VECTOR CorrelationVector)
{
    CHAR Digits[11];
    ULONG Length, Start, Index, Count;
    ULONG64 Value = 0;

    Length = RtlpCvValidate(CorrelationVector);
    if (Length == 0)
        return STATUS_INVALID_PARAMETER;

    for (Start = Length; CorrelationVector->Vector[Start - 1] != '.'; Start--);
    for (Index = Start; Index < Length; Index++)
        Value = Value * 10 + (CorrelationVector->Vector[Index] - '0');

    /* Each element is a 32-bit value */
    if (Length - Start > 10 || Value >= MAXULONG)
        return STATUS_BUFFER_OVERFLOW;
    Value++;

    for (Count = 0; Value != 0 || Count == 0; Value /= 10)
        Digits[Count++] = (CHAR)('0' + (Value % 10));

    if (Start + Count > RtlpCvMaxLength(CorrelationVector->Version))
        return STATUS_BUFFER_OVERFLOW;

    for (Index = 0; Index < Count; Index++)
        CorrelationVector->Vector[Start + Index] = Digits[Count - 1 - Index];
    CorrelationVector->Vector[Start + Count] = ANSI_NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlExtendCorrelationVector(
    _Inout_ PCORRELATION_VECTOR CorrelationVector)
{
    ULONG Length = RtlpCvValidate(CorrelationVector);

    if (Length == 0 || Length + 2 > RtlpCvMaxLength(CorrelationVector->Version))
        return STATUS_INVALID_PARAMETER;

    CorrelationVector->Vector[Length] = '.';
    CorrelationVector->Vector[Length + 1] = '0';
    CorrelationVector->Vector[Length + 2] = ANSI_NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlValidateCorrelationVector(
    _In_ PCORRELATION_VECTOR Vector)
{
    if (Vector == NULL || RtlpCvValidate(Vector) == 0)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

/* MEMORY ********************************************************************/

VOID
NTAPI
RtlFillMemoryNonTemporal(
    _Out_writes_bytes_all_(Length) VOID UNALIGNED *Destination,
    _In_ SIZE_T Length,
    _In_ const UCHAR Value)
{
#if defined(_M_AMD64)
    PUCHAR Current = Destination;
    ULONG64 Pattern = 0x0101010101010101ULL * Value;

    /* Non-temporal stores for 8 bytes and more, as documented; MOVNTI needs an aligned quadword */
    if (Length >= 8)
    {
        while (((ULONG_PTR)Current & 7) != 0)
        {
            *Current++ = Value;
            Length--;
        }
        while (Length >= 8)
        {
            _mm_stream_si64((long long *)Current, (long long)Pattern);
            Current += 8;
            Length -= 8;
        }
        _mm_sfence();
    }
    RtlFillMemory(Current, Length, Value);
#else
    RtlFillMemory(Destination, Length, Value);
#endif
}

/* CLOUD FILES ***************************************************************/

BOOLEAN
NTAPI
RtlIsCloudFilesPlaceholder(
    _In_ ULONG FileAttributes,
    _In_ ULONG ReparseTag)
{
    /* Without the reparse attribute the second value is an EA size, not a tag */
    return (FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           ((ReparseTag & ~IO_REPARSE_TAG_CLOUD_MASK) == IO_REPARSE_TAG_CLOUD);
}

/* REGISTRY ******************************************************************/

#define TAG_RTL_REGISTRY_FALLBACK 'bFgR'

static
NTSTATUS
RtlpQueryRegistryValueData(
    _In_ HANDLE KeyHandle,
    _In_ PUNICODE_STRING ValueName,
    _In_ ULONG ValueLength,
    _Out_opt_ PULONG ValueType,
    _Out_writes_bytes_to_(ValueLength, *ResultLength) PVOID ValueData,
    _Out_ PULONG ResultLength)
{
    PKEY_VALUE_PARTIAL_INFORMATION Info;
    ULONG InfoLength;
    ULONG Returned;
    NTSTATUS Status;

    InfoLength = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + ValueLength;
    if (InfoLength < ValueLength)
        return STATUS_INVALID_PARAMETER;

    Info = ExAllocatePoolWithTag(PagedPool, InfoLength, TAG_RTL_REGISTRY_FALLBACK);
    if (Info == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ZwQueryValueKey(KeyHandle, ValueName, KeyValuePartialInformation,
                             Info, InfoLength, &Returned);
    if (NT_SUCCESS(Status) || (Status == STATUS_BUFFER_OVERFLOW))
    {
        /* On overflow the size is the one the caller needs */
        if (ValueType != NULL)
            *ValueType = Info->Type;
        *ResultLength = Info->DataLength;
        if (NT_SUCCESS(Status))
            RtlCopyMemory(ValueData, Info->Data, Info->DataLength);
    }

    ExFreePoolWithTag(Info, TAG_RTL_REGISTRY_FALLBACK);
    return Status;
}

NTSTATUS
NTAPI
RtlQueryRegistryValueWithFallback(
    _In_opt_ HANDLE PrimaryHandle,
    _In_opt_ HANDLE FallbackHandle,
    _In_ PUNICODE_STRING ValueName,
    _In_ ULONG ValueLength,
    _Out_opt_ PULONG ValueType,
    _Out_writes_bytes_to_(ValueLength, *ResultLength) PVOID ValueData,
    _Out_ PULONG ResultLength)
{
    NTSTATUS Status = STATUS_OBJECT_NAME_NOT_FOUND;

    PAGED_CODE();

    if ((ValueName == NULL) || (ResultLength == NULL) ||
        ((ValueLength != 0) && (ValueData == NULL)) ||
        ((PrimaryHandle == NULL) && (FallbackHandle == NULL)))
        return STATUS_INVALID_PARAMETER;

    *ResultLength = 0;

    if (PrimaryHandle != NULL)
    {
        Status = RtlpQueryRegistryValueData(PrimaryHandle, ValueName, ValueLength,
                                            ValueType, ValueData, ResultLength);
    }

    /* Only a missing value falls back; any other failure is returned as it is */
    if ((Status == STATUS_OBJECT_NAME_NOT_FOUND) && (FallbackHandle != NULL))
    {
        Status = RtlpQueryRegistryValueData(FallbackHandle, ValueName, ValueLength,
                                            ValueType, ValueData, ResultLength);
    }

    return Status;
}

/* EOF */
