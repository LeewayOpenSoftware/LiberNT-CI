/*
 * PROJECT:     LiberNT Credential Serialization Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Packs, validates and maps credential provider logon serializations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <ntsecapi.h>
#include <credser.h>

typedef struct _CREDSER_RANGE
{
    ULONG Start;
    ULONG End;
} CREDSER_RANGE;

typedef struct _CREDSER_GENERIC_PACKED
{
    ULONG Reserved;
    GUID Type;
    UNICODE_STRING UserName;
    UNICODE_STRING Password;
} CREDSER_GENERIC_PACKED;

static const GUID CredSerGenericType =
    {0x2a818d91, 0x248e, 0x4af7, {0xb2, 0x0f, 0x09, 0xf6, 0xc1, 0xfd, 0x80, 0x76}};

static SIZE_T
CredSerStringBytes(_In_opt_ PCWSTR String)
{
    return String ? wcslen(String) * sizeof(WCHAR) : 0;
}

static BOOLEAN
CredSerFitsLimit(_In_ SIZE_T Bytes, _In_ ULONG MaxChars)
{
    return Bytes <= MaxChars * sizeof(WCHAR);
}

static SIZE_T
CredSerPackedBytes(_In_ SIZE_T Bytes, _In_ BOOLEAN Optional)
{
    return (Optional && !Bytes) ? 0 : Bytes + sizeof(WCHAR);
}

static VOID
CredSerPutString(
    _Inout_ PUCHAR Base,
    _Inout_ PULONG Offset,
    _Out_ PUNICODE_STRING Target,
    _In_opt_ PCWSTR Source,
    _In_ SIZE_T Bytes,
    _In_ BOOLEAN Optional)
{
    SIZE_T Packed = CredSerPackedBytes(Bytes, Optional);

    Target->Length = (USHORT)Bytes;
    Target->MaximumLength = (USHORT)Packed;
    Target->Buffer = Packed ? (PWSTR)(ULONG_PTR)*Offset : NULL;
    if (Packed)
    {
        CopyMemory(Base + *Offset, Source, Bytes);
        *(PWCHAR)(Base + *Offset + Bytes) = UNICODE_NULL;
        *Offset += (ULONG)Packed;
    }
}

NTSTATUS
CredSerPackLogon(
    _In_ KERB_LOGON_SUBMIT_TYPE MessageType,
    _In_opt_ PCWSTR LogonDomainName,
    _In_ PCWSTR UserName,
    _In_ PCWSTR Password,
    _In_opt_ const LUID *LogonId,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    PKERB_INTERACTIVE_UNLOCK_LOGON Unlock;
    SIZE_T DomainBytes, UserBytes, PasswordBytes, Total;
    ULONG Offset;

    if (!UserName || !Password || !BufferSize)
        return STATUS_INVALID_PARAMETER;
    if (MessageType != KerbInteractiveLogon && MessageType != KerbWorkstationUnlockLogon)
        return STATUS_INVALID_PARAMETER;

    DomainBytes = CredSerStringBytes(LogonDomainName);
    UserBytes = CredSerStringBytes(UserName);
    PasswordBytes = CredSerStringBytes(Password);
    if (!UserBytes ||
        !CredSerFitsLimit(DomainBytes, CREDSER_MAX_NAME_CHARS) ||
        !CredSerFitsLimit(UserBytes, CREDSER_MAX_NAME_CHARS) ||
        !CredSerFitsLimit(PasswordBytes, CREDSER_MAX_PASSWORD_CHARS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Total = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + CredSerPackedBytes(DomainBytes, TRUE) +
            CredSerPackedBytes(UserBytes, FALSE) + CredSerPackedBytes(PasswordBytes, FALSE);
    if (!Buffer || *BufferSize < Total)
    {
        *BufferSize = (ULONG)Total;
        return STATUS_BUFFER_TOO_SMALL;
    }

    ZeroMemory(Buffer, Total);
    Unlock = Buffer;
    Unlock->Logon.MessageType = MessageType;
    if (LogonId)
        Unlock->LogonId = *LogonId;

    Offset = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
    CredSerPutString(Buffer, &Offset, &Unlock->Logon.LogonDomainName, LogonDomainName, DomainBytes, TRUE);
    CredSerPutString(Buffer, &Offset, &Unlock->Logon.UserName, UserName, UserBytes, FALSE);
    CredSerPutString(Buffer, &Offset, &Unlock->Logon.Password, Password, PasswordBytes, FALSE);

    *BufferSize = (ULONG)Total;
    return STATUS_SUCCESS;
}

NTSTATUS
CredSerPackChangePassword(
    _In_opt_ PCWSTR DomainName,
    _In_ PCWSTR AccountName,
    _In_ PCWSTR OldPassword,
    _In_ PCWSTR NewPassword,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    PKERB_CHANGEPASSWORD_REQUEST Request;
    SIZE_T DomainBytes, AccountBytes, OldBytes, NewBytes, Total;
    ULONG Offset;

    if (!AccountName || !OldPassword || !NewPassword || !BufferSize)
        return STATUS_INVALID_PARAMETER;

    DomainBytes = CredSerStringBytes(DomainName);
    AccountBytes = CredSerStringBytes(AccountName);
    OldBytes = CredSerStringBytes(OldPassword);
    NewBytes = CredSerStringBytes(NewPassword);
    if (!AccountBytes ||
        !CredSerFitsLimit(DomainBytes, CREDSER_MAX_NAME_CHARS) ||
        !CredSerFitsLimit(AccountBytes, CREDSER_MAX_NAME_CHARS) ||
        !CredSerFitsLimit(OldBytes, CREDSER_MAX_PASSWORD_CHARS) ||
        !CredSerFitsLimit(NewBytes, CREDSER_MAX_PASSWORD_CHARS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Total = sizeof(KERB_CHANGEPASSWORD_REQUEST) + CredSerPackedBytes(DomainBytes, TRUE) +
            CredSerPackedBytes(AccountBytes, FALSE) + CredSerPackedBytes(OldBytes, FALSE) +
            CredSerPackedBytes(NewBytes, FALSE);
    if (!Buffer || *BufferSize < Total)
    {
        *BufferSize = (ULONG)Total;
        return STATUS_BUFFER_TOO_SMALL;
    }

    ZeroMemory(Buffer, Total);
    Request = Buffer;
    Request->MessageType = KerbChangePasswordMessage;
    Request->Impersonating = FALSE;

    Offset = sizeof(KERB_CHANGEPASSWORD_REQUEST);
    CredSerPutString(Buffer, &Offset, &Request->DomainName, DomainName, DomainBytes, TRUE);
    CredSerPutString(Buffer, &Offset, &Request->AccountName, AccountName, AccountBytes, FALSE);
    CredSerPutString(Buffer, &Offset, &Request->OldPassword, OldPassword, OldBytes, FALSE);
    CredSerPutString(Buffer, &Offset, &Request->NewPassword, NewPassword, NewBytes, FALSE);

    *BufferSize = (ULONG)Total;
    return STATUS_SUCCESS;
}

static VOID
CredSerPutRaw(
    _Inout_ PUCHAR Base,
    _Inout_ PULONG Offset,
    _Out_ PUNICODE_STRING Target,
    _In_ PCWSTR Source,
    _In_ SIZE_T Bytes)
{
    Target->Length = Target->MaximumLength = (USHORT)Bytes;
    Target->Buffer = (PWSTR)(ULONG_PTR)*Offset;
    CopyMemory(Base + *Offset, Source, Bytes);
    *Offset += (ULONG)Bytes;
}

NTSTATUS
CredSerPackGeneric(
    _In_ PCWSTR UserName,
    _In_ PCWSTR Password,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize)
{
    CREDSER_GENERIC_PACKED *Packed;
    SIZE_T UserBytes, PasswordBytes, Total;
    ULONG Offset;

    if (!UserName || !Password || !BufferSize)
        return STATUS_INVALID_PARAMETER;

    UserBytes = CredSerStringBytes(UserName);
    PasswordBytes = CredSerStringBytes(Password);
    if (!CredSerFitsLimit(UserBytes, CREDSER_MAX_GENERIC_USER_CHARS) ||
        !CredSerFitsLimit(PasswordBytes, CREDSER_MAX_GENERIC_PASSWORD_CHARS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Total = sizeof(CREDSER_GENERIC_PACKED) + UserBytes + PasswordBytes;
    if (!Buffer || *BufferSize < Total)
    {
        *BufferSize = (ULONG)Total;
        return STATUS_BUFFER_TOO_SMALL;
    }

    ZeroMemory(Buffer, Total);
    Packed = Buffer;
    Packed->Type = CredSerGenericType;
    Offset = sizeof(CREDSER_GENERIC_PACKED);
    CredSerPutRaw(Buffer, &Offset, &Packed->UserName, UserName, UserBytes);
    CredSerPutRaw(Buffer, &Offset, &Packed->Password, Password, PasswordBytes);

    *BufferSize = (ULONG)Total;
    return STATUS_SUCCESS;
}

NTSTATUS
CredSerCapture(
    _In_reads_bytes_(Size) const VOID *Source,
    _In_ ULONG Size,
    _Outptr_ PVOID *Copy)
{
    PVOID Private;

    if (!Copy)
        return STATUS_INVALID_PARAMETER;
    *Copy = NULL;
    if (!Source || !Size || Size > CREDSER_MAX_BLOB_SIZE)
        return STATUS_INVALID_PARAMETER;

    Private = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Private)
        return STATUS_NO_MEMORY;
    CopyMemory(Private, Source, Size);
    *Copy = Private;
    return STATUS_SUCCESS;
}

VOID
CredSerFree(
    _In_opt_ _Post_invalid_ PVOID Copy,
    _In_ ULONG Size)
{
    if (!Copy)
        return;
    SecureZeroMemory(Copy, Size);
    HeapFree(GetProcessHeap(), 0, Copy);
}

static NTSTATUS
CredSerCheckString(
    _In_ const UCHAR *Base,
    _In_ ULONG Size,
    _In_ ULONG HeaderSize,
    _In_ const UNICODE_STRING *Packed,
    _In_ ULONG MaxChars,
    _In_ BOOLEAN Required,
    _In_ BOOLEAN RejectNul,
    _Out_ CREDSER_RANGE *Range,
    _Out_ PUNICODE_STRING Unpacked)
{
    ULONG_PTR Offset = (ULONG_PTR)Packed->Buffer;
    USHORT i;

    Range->Start = Range->End = 0;
    ZeroMemory(Unpacked, sizeof(*Unpacked));

    if (Packed->Length > Packed->MaximumLength || (Packed->Length & 1) || (Packed->MaximumLength & 1))
        return STATUS_INVALID_PARAMETER;
    if (Packed->Length > MaxChars * sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;
    if (!Packed->Length)
    {
        if (Required)
            return STATUS_INVALID_PARAMETER;
        if (!Packed->MaximumLength && !Offset)
            return STATUS_SUCCESS;
    }

    if ((Offset & 1) || Offset < HeaderSize || Offset > Size || Packed->MaximumLength > Size - Offset)
        return STATUS_INVALID_PARAMETER;

    Unpacked->Buffer = (PWSTR)(Base + Offset);
    Unpacked->Length = Packed->Length;
    Unpacked->MaximumLength = Packed->Length;
    if (RejectNul)
    {
        for (i = 0; i < Packed->Length / sizeof(WCHAR); i++)
        {
            if (!Unpacked->Buffer[i])
                return STATUS_INVALID_PARAMETER;
        }
    }

    Range->Start = (ULONG)Offset;
    Range->End = (ULONG)Offset + Packed->MaximumLength;
    return STATUS_SUCCESS;
}

static BOOLEAN
CredSerRangesDisjoint(_In_reads_(Count) const CREDSER_RANGE *Ranges, _In_ ULONG Count)
{
    ULONG i, j;

    for (i = 0; i < Count; i++)
    {
        if (Ranges[i].Start == Ranges[i].End)
            continue;
        for (j = i + 1; j < Count; j++)
        {
            if (Ranges[j].Start == Ranges[j].End)
                continue;
            if (Ranges[i].Start < Ranges[j].End && Ranges[j].Start < Ranges[i].End)
                return FALSE;
        }
    }
    return TRUE;
}

NTSTATUS
CredSerUnpackLogon(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_LOGON Logon)
{
    const KERB_INTERACTIVE_UNLOCK_LOGON *Packed = Copy;
    CREDSER_RANGE Ranges[3];
    ULONG HeaderSize;
    NTSTATUS Status;

    if (!Logon)
        return STATUS_INVALID_PARAMETER;
    ZeroMemory(Logon, sizeof(*Logon));
    if (!Copy || Size < sizeof(KERB_INTERACTIVE_LOGON) || Size > CREDSER_MAX_BLOB_SIZE)
        return STATUS_INVALID_PARAMETER;

    switch (Packed->Logon.MessageType)
    {
        case KerbInteractiveLogon:
            HeaderSize = sizeof(KERB_INTERACTIVE_LOGON);
            break;
        case KerbWorkstationUnlockLogon:
            if (Size < sizeof(KERB_INTERACTIVE_UNLOCK_LOGON))
                return STATUS_INVALID_PARAMETER;
            HeaderSize = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
            break;
        default:
            return STATUS_INVALID_PARAMETER;
    }

    Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->Logon.LogonDomainName,
                                CREDSER_MAX_NAME_CHARS, FALSE, TRUE, &Ranges[0], &Logon->LogonDomainName);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->Logon.UserName,
                                    CREDSER_MAX_NAME_CHARS, TRUE, TRUE, &Ranges[1], &Logon->UserName);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->Logon.Password,
                                    CREDSER_MAX_PASSWORD_CHARS, FALSE, FALSE, &Ranges[2], &Logon->Password);
    if (NT_SUCCESS(Status) && !CredSerRangesDisjoint(Ranges, ARRAYSIZE(Ranges)))
        Status = STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(Status))
    {
        ZeroMemory(Logon, sizeof(*Logon));
        return Status;
    }

    Logon->MessageType = Packed->Logon.MessageType;
    if (Logon->MessageType == KerbWorkstationUnlockLogon)
        Logon->LogonId = Packed->LogonId;
    return STATUS_SUCCESS;
}

NTSTATUS
CredSerUnpackChangePassword(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_CHANGE_PASSWORD Change)
{
    const KERB_CHANGEPASSWORD_REQUEST *Packed = Copy;
    CREDSER_RANGE Ranges[4];
    const ULONG HeaderSize = sizeof(KERB_CHANGEPASSWORD_REQUEST);
    NTSTATUS Status;

    if (!Change)
        return STATUS_INVALID_PARAMETER;
    ZeroMemory(Change, sizeof(*Change));
    if (!Copy || Size < HeaderSize || Size > CREDSER_MAX_BLOB_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (Packed->MessageType != KerbChangePasswordMessage || Packed->Impersonating)
        return STATUS_INVALID_PARAMETER;

    Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->DomainName,
                                CREDSER_MAX_NAME_CHARS, FALSE, TRUE, &Ranges[0], &Change->DomainName);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->AccountName,
                                    CREDSER_MAX_NAME_CHARS, TRUE, TRUE, &Ranges[1], &Change->AccountName);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->OldPassword,
                                    CREDSER_MAX_PASSWORD_CHARS, FALSE, FALSE, &Ranges[2], &Change->OldPassword);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->NewPassword,
                                    CREDSER_MAX_PASSWORD_CHARS, FALSE, FALSE, &Ranges[3], &Change->NewPassword);
    if (NT_SUCCESS(Status) && !CredSerRangesDisjoint(Ranges, ARRAYSIZE(Ranges)))
        Status = STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(Status))
        ZeroMemory(Change, sizeof(*Change));
    return Status;
}

BOOLEAN
CredSerIsGeneric(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size)
{
    const CREDSER_GENERIC_PACKED *Packed = Copy;

    return Copy && Size >= sizeof(CREDSER_GENERIC_PACKED) && !Packed->Reserved &&
           !memcmp(&Packed->Type, &CredSerGenericType, sizeof(GUID));
}

NTSTATUS
CredSerUnpackGeneric(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_GENERIC Generic)
{
    const CREDSER_GENERIC_PACKED *Packed = Copy;
    CREDSER_RANGE Ranges[2];
    const ULONG HeaderSize = sizeof(CREDSER_GENERIC_PACKED);
    NTSTATUS Status;

    if (!Generic)
        return STATUS_INVALID_PARAMETER;
    ZeroMemory(Generic, sizeof(*Generic));
    if (Size > CREDSER_MAX_BLOB_SIZE || !CredSerIsGeneric(Copy, Size))
        return STATUS_INVALID_PARAMETER;

    Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->UserName, CREDSER_MAX_GENERIC_USER_CHARS,
                                FALSE, TRUE, &Ranges[0], &Generic->UserName);
    if (NT_SUCCESS(Status))
        Status = CredSerCheckString(Copy, Size, HeaderSize, &Packed->Password, CREDSER_MAX_GENERIC_PASSWORD_CHARS,
                                    FALSE, FALSE, &Ranges[1], &Generic->Password);
    if (NT_SUCCESS(Status) && !CredSerRangesDisjoint(Ranges, ARRAYSIZE(Ranges)))
        Status = STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(Status))
        ZeroMemory(Generic, sizeof(*Generic));
    return Status;
}

NTSTATUS
CredSerBuildMsv1_0Logon(
    _In_ const CREDSER_LOGON *Logon,
    _Outptr_result_bytebuffer_(*Size) PMSV1_0_INTERACTIVE_LOGON *Msv,
    _Out_ PULONG Size)
{
    PMSV1_0_INTERACTIVE_LOGON Out;
    PUCHAR Cursor;
    ULONG Total;

    if (!Logon || !Msv || !Size)
        return STATUS_INVALID_PARAMETER;
    *Msv = NULL;
    *Size = 0;
    if (Logon->MessageType != KerbInteractiveLogon && Logon->MessageType != KerbWorkstationUnlockLogon)
        return STATUS_INVALID_PARAMETER;

    Total = sizeof(*Out) + Logon->LogonDomainName.Length + Logon->UserName.Length + Logon->Password.Length;
    Out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Total);
    if (!Out)
        return STATUS_NO_MEMORY;

    Out->MessageType = MsV1_0InteractiveLogon;
    Cursor = (PUCHAR)(Out + 1);

    Out->LogonDomainName.Length = Out->LogonDomainName.MaximumLength = Logon->LogonDomainName.Length;
    Out->LogonDomainName.Buffer = Logon->LogonDomainName.Length ? (PWSTR)Cursor : NULL;
    CopyMemory(Cursor, Logon->LogonDomainName.Buffer, Logon->LogonDomainName.Length);
    Cursor += Logon->LogonDomainName.Length;

    Out->UserName.Length = Out->UserName.MaximumLength = Logon->UserName.Length;
    Out->UserName.Buffer = (PWSTR)Cursor;
    CopyMemory(Cursor, Logon->UserName.Buffer, Logon->UserName.Length);
    Cursor += Logon->UserName.Length;

    Out->Password.Length = Out->Password.MaximumLength = Logon->Password.Length;
    Out->Password.Buffer = Logon->Password.Length ? (PWSTR)Cursor : NULL;
    CopyMemory(Cursor, Logon->Password.Buffer, Logon->Password.Length);

    *Msv = Out;
    *Size = Total;
    return STATUS_SUCCESS;
}
