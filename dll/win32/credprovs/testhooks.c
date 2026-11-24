/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test-only access to credential secrets, built into credprovs_test.dll only
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#ifndef CREDPROVS_TEST_HOOKS
#error testhooks.c belongs to credprovs_test.dll only
#endif

HRESULT WINAPI
CredProvsTestGetSecretBuffer(
    _In_ ICredentialProviderCredential *Credential,
    _In_ DWORD Field,
    _Out_ const WCHAR **Buffer,
    _Out_ SIZE_T *Bytes)
{
    PWCREDENTIAL *This;

    if (!Credential || !Buffer || !Bytes || Field < PWFIELD_PASSWORD || Field > PWFIELD_CONFIRM_PASSWORD)
        return E_INVALIDARG;
    This = CONTAINING_RECORD(Credential, PWCREDENTIAL, ICredentialProviderCredential2_iface);
    *Buffer = This->Text[Field];
    *Bytes = sizeof(This->Text[Field]);
    return S_OK;
}
