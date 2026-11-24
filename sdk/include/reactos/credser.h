/*
 * PROJECT:     LiberNT Credential Serialization Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Packs, validates and maps credential provider logon serializations
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CREDSER_MAX_NAME_CHARS      256
#define CREDSER_MAX_PASSWORD_CHARS  256
#define CREDSER_MAX_BLOB_SIZE       (64 * 1024)
#define CREDSER_MAX_GENERIC_USER_CHARS      513
#define CREDSER_MAX_GENERIC_PASSWORD_CHARS  1280

typedef struct _CREDSER_GENERIC
{
    UNICODE_STRING UserName;
    UNICODE_STRING Password;
} CREDSER_GENERIC, *PCREDSER_GENERIC;

typedef struct _CREDSER_PACKAGES
{
    ULONG Negotiate;
    ULONG Msv1_0;
    BOOLEAN HaveNegotiate;
    BOOLEAN HaveMsv1_0;
} CREDSER_PACKAGES, *PCREDSER_PACKAGES;

typedef struct _CREDSER_LOGON
{
    KERB_LOGON_SUBMIT_TYPE MessageType;
    UNICODE_STRING LogonDomainName;
    UNICODE_STRING UserName;
    UNICODE_STRING Password;
    LUID LogonId;
} CREDSER_LOGON, *PCREDSER_LOGON;

typedef struct _CREDSER_CHANGE_PASSWORD
{
    UNICODE_STRING DomainName;
    UNICODE_STRING AccountName;
    UNICODE_STRING OldPassword;
    UNICODE_STRING NewPassword;
} CREDSER_CHANGE_PASSWORD, *PCREDSER_CHANGE_PASSWORD;

NTSTATUS
CredSerLookupPackages(
    _In_ HANDLE LsaHandle,
    _Out_ PCREDSER_PACKAGES Packages);

ULONG
CredSerPreferredPackage(
    _In_ const CREDSER_PACKAGES *Packages);

BOOLEAN
CredSerIsKnownPackage(
    _In_ const CREDSER_PACKAGES *Packages,
    _In_ ULONG Package);

NTSTATUS
CredSerPackLogon(
    _In_ KERB_LOGON_SUBMIT_TYPE MessageType,
    _In_opt_ PCWSTR LogonDomainName,
    _In_ PCWSTR UserName,
    _In_ PCWSTR Password,
    _In_opt_ const LUID *LogonId,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize);

NTSTATUS
CredSerPackChangePassword(
    _In_opt_ PCWSTR DomainName,
    _In_ PCWSTR AccountName,
    _In_ PCWSTR OldPassword,
    _In_ PCWSTR NewPassword,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize);

/* The credui generic-credential layout (CRED_PACK_GENERIC_CREDENTIALS). */
NTSTATUS
CredSerPackGeneric(
    _In_ PCWSTR UserName,
    _In_ PCWSTR Password,
    _Out_writes_bytes_opt_(*BufferSize) PVOID Buffer,
    _Inout_ PULONG BufferSize);

/* Callers capture untrusted input once and unpack only the copy. */
NTSTATUS
CredSerCapture(
    _In_reads_bytes_(Size) const VOID *Source,
    _In_ ULONG Size,
    _Outptr_ PVOID *Copy);

VOID
CredSerFree(
    _In_opt_ _Post_invalid_ PVOID Copy,
    _In_ ULONG Size);

/* The unpacked strings point into Copy and are invalid after CredSerFree(Copy). */
NTSTATUS
CredSerUnpackLogon(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_LOGON Logon);

NTSTATUS
CredSerUnpackChangePassword(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_CHANGE_PASSWORD Change);

BOOLEAN
CredSerIsGeneric(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size);

NTSTATUS
CredSerUnpackGeneric(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ ULONG Size,
    _Out_ PCREDSER_GENERIC Generic);

/* Free *Msv with CredSerFree(*Msv, *Size). */
NTSTATUS
CredSerBuildMsv1_0Logon(
    _In_ const CREDSER_LOGON *Logon,
    _Outptr_result_bytebuffer_(*Size) PMSV1_0_INTERACTIVE_LOGON *Msv,
    _Out_ PULONG Size);

#ifdef __cplusplus
}
#endif
