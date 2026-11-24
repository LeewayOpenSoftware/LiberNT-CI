/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Password credential provider definitions
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#define COBJMACROS
#define CONST_VTABLE

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <objbase.h>
#include <wincred.h>
#include <ntsecapi.h>
#include <credentialprovider.h>
#include <shlguid.h>
#include <propkey.h>
#include <credser.h>
#include <wine/debug.h>

#include "resource.h"

enum
{
    PWFIELD_FRIENDLY_NAME,
    PWFIELD_USER_NAME,
    PWFIELD_PASSWORD,
    PWFIELD_NEW_PASSWORD,
    PWFIELD_CONFIRM_PASSWORD,
    PWFIELD_SUBMIT,
    PWFIELD_PROVIDER_LABEL,
    PWFIELD_COUNT
};

#define PWFIELD_TEXT_CHARS  (CREDSER_MAX_NAME_CHARS + 1)

typedef struct _PWPROVIDER PWPROVIDER;

typedef struct _PWCREDENTIAL
{
    ICredentialProviderCredential2 ICredentialProviderCredential2_iface;
    ICredentialProviderCredentialWithFieldOptions ICredentialProviderCredentialWithFieldOptions_iface;
    LONG Ref;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario;
    ICredentialProviderCredentialEvents *Events;
    BOOL KnownUser;
    LPWSTR Sid;
    WCHAR Text[PWFIELD_COUNT][PWFIELD_TEXT_CHARS];
} PWCREDENTIAL;

extern HINSTANCE CredProvsInstance;
extern LONG CredProvsObjects;

HRESULT PasswordProvider_Create(_In_ REFIID riid, _Outptr_ void **ppv);
HRESULT PasswordCredential_Create(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                                  _In_opt_ ICredentialProviderUser *User,
                                  _In_opt_ PCWSTR PrefillUserName,
                                  _Outptr_ PWCREDENTIAL **Credential);
BOOL PasswordProvider_IsKnownScenario(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario);
HRESULT PasswordProvider_DupString(_In_ PCWSTR Source, _Outptr_ LPWSTR *Out);
