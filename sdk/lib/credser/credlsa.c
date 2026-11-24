/*
 * PROJECT:     LiberNT Credential Serialization Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Looks up the authentication packages a serialization may name
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <ntsecapi.h>
#include <security.h>
#include <credser.h>

static VOID
CredSerInitLsaString(_Out_ PLSA_STRING String, _In_ PCSTR Source)
{
    String->Buffer = (PCHAR)Source;
    String->Length = (USHORT)strlen(Source);
    String->MaximumLength = String->Length + 1;
}

NTSTATUS
CredSerLookupPackages(
    _In_ HANDLE LsaHandle,
    _Out_ PCREDSER_PACKAGES Packages)
{
    LSA_STRING Name;
    NTSTATUS Status;

    ZeroMemory(Packages, sizeof(*Packages));

    CredSerInitLsaString(&Name, NEGOSSP_NAME_A);
    Status = LsaLookupAuthenticationPackage(LsaHandle, &Name, &Packages->Negotiate);
    Packages->HaveNegotiate = NT_SUCCESS(Status);

    CredSerInitLsaString(&Name, MSV1_0_PACKAGE_NAME);
    Status = LsaLookupAuthenticationPackage(LsaHandle, &Name, &Packages->Msv1_0);
    Packages->HaveMsv1_0 = NT_SUCCESS(Status);

    if (!Packages->HaveNegotiate && !Packages->HaveMsv1_0)
        return Status;
    return STATUS_SUCCESS;
}

ULONG
CredSerPreferredPackage(
    _In_ const CREDSER_PACKAGES *Packages)
{
    return Packages->HaveNegotiate ? Packages->Negotiate : Packages->Msv1_0;
}

BOOLEAN
CredSerIsKnownPackage(
    _In_ const CREDSER_PACKAGES *Packages,
    _In_ ULONG Package)
{
    return (Packages->HaveNegotiate && Package == Packages->Negotiate) ||
           (Packages->HaveMsv1_0 && Package == Packages->Msv1_0);
}
