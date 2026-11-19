/*
 * PROJECT:     LiberNT Runtime Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     UTF-8 counted string routines
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <rtl_vista.h>

VOID
NTAPI
RtlInitUTF8String(OUT PUTF8_STRING DestinationString,
                  IN PCSZ SourceString)
{
    RtlInitAnsiString(DestinationString, SourceString);
}

NTSTATUS
NTAPI
RtlUTF8StringToUnicodeString(
    IN OUT PUNICODE_STRING DestinationString,
    IN PUTF8_STRING SourceString,
    IN BOOLEAN AllocateDestinationString)
{
    NTSTATUS Status;
    ULONG Length;
    ULONG Written;

    PAGED_CODE_RTL();

    Status = RtlUTF8ToUnicodeN(NULL, 0, &Length, SourceString->Buffer, SourceString->Length);
    if (!NT_SUCCESS(Status)) return Status;
    if (Length + sizeof(WCHAR) > MAXUSHORT) return STATUS_INVALID_PARAMETER_2;
    DestinationString->Length = (USHORT)Length;

    if (AllocateDestinationString)
    {
        DestinationString->Buffer = RtlpAllocateStringMemory(Length + sizeof(WCHAR), TAG_USTR);
        DestinationString->MaximumLength = (USHORT)(Length + sizeof(WCHAR));
        if (!DestinationString->Buffer) return STATUS_NO_MEMORY;
    }
    else if (DestinationString->Length >= DestinationString->MaximumLength)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    Status = RtlUTF8ToUnicodeN(DestinationString->Buffer,
                               DestinationString->Length,
                               &Written,
                               SourceString->Buffer,
                               SourceString->Length);
    if (!NT_SUCCESS(Status))
    {
        if (AllocateDestinationString)
        {
            RtlpFreeStringMemory(DestinationString->Buffer, TAG_USTR);
            DestinationString->Buffer = NULL;
        }
        return Status;
    }

    DestinationString->Buffer[Written / sizeof(WCHAR)] = UNICODE_NULL;
    return Status;
}


NTSTATUS
NTAPI
RtlInitUTF8StringEx(
    _Out_ PUTF8_STRING DestinationString,
    _In_opt_z_ __drv_aliasesMem PCSZ SourceString)
{
    return RtlInitAnsiStringEx(DestinationString, SourceString);
}

NTSTATUS
NTAPI
RtlUnicodeStringToUTF8String(
    _Inout_ PUTF8_STRING DestinationString,
    _In_ PCUNICODE_STRING SourceString,
    _In_ BOOLEAN AllocateDestinationString)
{
    NTSTATUS Status;
    NTSTATUS ConvertStatus;
    ULONG Length;
    ULONG Written;

    PAGED_CODE_RTL();

    if ((DestinationString == NULL) || (SourceString == NULL) ||
        (SourceString->Length % sizeof(WCHAR)) ||
        ((SourceString->Length != 0) && (SourceString->Buffer == NULL)))
        return STATUS_INVALID_PARAMETER;

    /* Exactly Length bytes are converted, so the result ends in a null only if the source does */
    if (SourceString->Length == 0)
    {
        Length = 0;
        ConvertStatus = STATUS_SUCCESS;
    }
    else
    {
        ConvertStatus = RtlUnicodeToUTF8N(NULL, 0, &Length, SourceString->Buffer, SourceString->Length);
        if (!NT_SUCCESS(ConvertStatus)) return ConvertStatus;
    }
    if (Length > MAXUSHORT) return STATUS_INVALID_PARAMETER_2;

    if (AllocateDestinationString)
    {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
        if (Length == 0) return STATUS_SUCCESS;

        DestinationString->Buffer = RtlpAllocateStringMemory(Length, TAG_ASTR);
        if (DestinationString->Buffer == NULL) return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)Length;
    }
    else if (Length > DestinationString->MaximumLength)
    {
        /* Convert what fits; the converter only writes whole characters */
        Written = 0;
        if (DestinationString->MaximumLength != 0)
        {
            RtlUnicodeToUTF8N(DestinationString->Buffer, DestinationString->MaximumLength,
                              &Written, SourceString->Buffer, SourceString->Length);
        }
        DestinationString->Length = (USHORT)Written;
        return STATUS_BUFFER_OVERFLOW;
    }

    if (Length == 0)
    {
        DestinationString->Length = 0;
        return STATUS_SUCCESS;
    }

    Status = RtlUnicodeToUTF8N(DestinationString->Buffer, Length, &Written,
                               SourceString->Buffer, SourceString->Length);
    if (!NT_SUCCESS(Status))
    {
        if (AllocateDestinationString)
        {
            RtlpFreeStringMemory(DestinationString->Buffer, TAG_ASTR);
            DestinationString->Buffer = NULL;
            DestinationString->MaximumLength = 0;
        }
        return Status;
    }

    DestinationString->Length = (USHORT)Written;
    return ConvertStatus;
}

VOID
NTAPI
RtlFreeUTF8String(
    _Inout_ PUTF8_STRING Utf8String)
{
    PAGED_CODE_RTL();

    if ((Utf8String != NULL) && (Utf8String->Buffer != NULL))
    {
        RtlpFreeStringMemory(Utf8String->Buffer, TAG_ASTR);
        RtlZeroMemory(Utf8String, sizeof(*Utf8String));
    }
}
