/*
 * PROJECT:     LiberNT Credential Provider Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Private interface for enumerating credential providers and filters
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <credentialprovider.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPH_FLAG_FALLBACK           0x00000001
#define CPH_MAX_PROVIDERS           64

#define CPH_SKIP_DISABLED           0x00000001
#define CPH_SKIP_EXCLUDED           0x00000002
#define CPH_SKIP_FILTERED           0x00000004
#define CPH_SKIP_LOAD_FAILED        0x00000008
#define CPH_SKIP_NO_SCENARIO        0x00000010

typedef struct _CPH_PROVIDER
{
    CLSID Clsid;
    ICredentialProvider *Provider;
} CPH_PROVIDER, *PCPH_PROVIDER;

typedef struct _CPH_SKIPPED
{
    CLSID Clsid;
    ULONG Reason;
    HRESULT Result;
} CPH_SKIPPED, *PCPH_SKIPPED;

typedef struct _CPH_PROVIDER_SET
{
    ULONG Count;
    ULONG SkippedCount;
    BOOL PolicyDeniesPassword;
    CPH_PROVIDER Providers[CPH_MAX_PROVIDERS];
    CPH_SKIPPED Skipped[CPH_MAX_PROVIDERS];
} CPH_PROVIDER_SET, *PCPH_PROVIDER_SET;

/* The caller owns COM initialization; release the set with CredProvHostFreeProviders. */
HRESULT WINAPI
CredProvHostEnumerateProviders(
    _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
    _In_ DWORD ScenarioFlags,
    _In_ DWORD HostFlags,
    _Outptr_ PCPH_PROVIDER_SET *Set);

VOID WINAPI
CredProvHostFreeProviders(
    _In_opt_ _Post_invalid_ PCPH_PROVIDER_SET Set);

HRESULT WINAPI
CredProvHostUpdateRemoteCredential(
    _In_ const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *In,
    _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *Out);

#ifdef __cplusplus
}
#endif
