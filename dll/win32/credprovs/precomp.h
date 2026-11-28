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

/* Field IDs follow the Win11 password provider FieldTable (sparse). */
enum
{
    PWFIELD_FRIENDLY_NAME = 0,
    PWFIELD_USER_NAME = 1,
    PWFIELD_PASSWORD = 2,
    PWFIELD_NEW_PASSWORD = 3,
    PWFIELD_CONFIRM_PASSWORD = 4,
    PWFIELD_SUBMIT = 12,
    PWFIELD_PROVIDER_LABEL = 15,
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
    BOOL BlankPassword;
    LPWSTR Sid;
    WCHAR Text[PWFIELD_COUNT][PWFIELD_TEXT_CHARS];
} PWCREDENTIAL;

extern HINSTANCE CredProvsInstance;
extern LONG CredProvsObjects;

BOOL PasswordProvider_IsKnownFieldId(_In_ DWORD Id);

HRESULT PasswordProvider_Create(_In_ REFIID riid, _Outptr_ void **ppv);
HRESULT PasswordCredential_Create(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                                  _In_opt_ ICredentialProviderUser *User,
                                  _In_opt_ PCWSTR PrefillUserName,
                                  _Outptr_ PWCREDENTIAL **Credential);
BOOL PasswordProvider_IsKnownScenario(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario);
BOOL PasswordProvider_SidHasBlankPassword(_In_ PCWSTR SidString);
HRESULT PasswordProvider_DupString(_In_ PCWSTR Source, _Outptr_ LPWSTR *Out);
