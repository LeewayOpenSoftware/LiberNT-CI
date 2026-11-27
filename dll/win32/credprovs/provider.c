/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Password credential provider
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(credprovs);

#define PWPROVIDER_MAX_TILES 64

static const GUID LocalUserProvider =
    { 0xa198529b, 0x730f, 0x4089, { 0xb6, 0x46, 0xa1, 0x25, 0x57, 0xf5, 0x66, 0x5e } };

struct _PWPROVIDER
{
    ICredentialProvider ICredentialProvider_iface;
    ICredentialProviderSetUserArray ICredentialProviderSetUserArray_iface;
    LONG Ref;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario;
    DWORD Flags;
    ICredentialProviderUserArray *Users;
    ICredentialProviderEvents *Events;
    UINT_PTR AdviseContext;
    WCHAR Prefill[PWFIELD_TEXT_CHARS];
    PWCREDENTIAL *Tiles[PWPROVIDER_MAX_TILES];
    DWORD TileCount;
};

static const struct
{
    DWORD Id;
    CREDENTIAL_PROVIDER_FIELD_TYPE Type;
    UINT Label;
    const GUID *FieldType;
} FieldTable[] =
{
    /* IDs 0-4, 12 and 15 are the Win11 password provider FieldTable. */
    { PWFIELD_FRIENDLY_NAME,    CPFT_LARGE_TEXT,      IDS_FRIENDLY_NAME,    &GUID_NULL },
    { PWFIELD_USER_NAME,        CPFT_EDIT_TEXT,       IDS_USERNAME,         &CPFG_LOGON_USERNAME },
    { PWFIELD_PASSWORD,         CPFT_PASSWORD_TEXT,   IDS_PASSWORD,         &CPFG_LOGON_PASSWORD },
    { PWFIELD_NEW_PASSWORD,     CPFT_PASSWORD_TEXT,   IDS_NEW_PASSWORD,     &GUID_NULL },
    { PWFIELD_CONFIRM_PASSWORD, CPFT_PASSWORD_TEXT,   IDS_CONFIRM_PASSWORD, &GUID_NULL },
    { PWFIELD_SUBMIT,           CPFT_SUBMIT_BUTTON,   IDS_SUBMIT,           &GUID_NULL },
    { PWFIELD_PROVIDER_LABEL,   CPFT_SMALL_TEXT,      IDS_PROVIDER_LABEL,   &CPFG_CREDENTIAL_PROVIDER_LABEL },
};

BOOL
PasswordProvider_IsKnownFieldId(_In_ DWORD Id)
{
    DWORD Index;

    for (Index = 0; Index < ARRAYSIZE(FieldTable); Index++)
    {
        if (FieldTable[Index].Id == Id)
            return TRUE;
    }
    return FALSE;
}

HRESULT
PasswordProvider_DupString(_In_ PCWSTR Source, _Outptr_ LPWSTR *Out)
{
    SIZE_T Bytes = (wcslen(Source) + 1) * sizeof(WCHAR);

    *Out = CoTaskMemAlloc(Bytes);
    if (!*Out)
        return E_OUTOFMEMORY;
    CopyMemory(*Out, Source, Bytes);
    return S_OK;
}

BOOL
PasswordProvider_IsKnownScenario(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario)
{
    return Scenario == CPUS_LOGON || Scenario == CPUS_UNLOCK_WORKSTATION ||
           Scenario == CPUS_CHANGE_PASSWORD || Scenario == CPUS_CREDUI;
}

static PWPROVIDER *
ProviderFrom(ICredentialProvider *iface)
{
    return CONTAINING_RECORD(iface, PWPROVIDER, ICredentialProvider_iface);
}

static PWPROVIDER *
ProviderFromSetUserArray(ICredentialProviderSetUserArray *iface)
{
    return CONTAINING_RECORD(iface, PWPROVIDER, ICredentialProviderSetUserArray_iface);
}

static VOID
ReleaseTiles(_Inout_ PWPROVIDER *This)
{
    while (This->TileCount)
    {
        This->TileCount--;
        ICredentialProviderCredential2_Release(&This->Tiles[This->TileCount]->ICredentialProviderCredential2_iface);
        This->Tiles[This->TileCount] = NULL;
    }
}

static HRESULT
BuildTiles(_Inout_ PWPROVIDER *This)
{
    ICredentialProviderUser *User;
    PWCREDENTIAL *Credential;
    DWORD Count = 0, i;
    HRESULT hr;

    ReleaseTiles(This);
    if (This->Users && This->Scenario != CPUS_CREDUI)
    {
        CREDENTIAL_PROVIDER_ACCOUNT_OPTIONS Options = CPAO_NONE;

        hr = ICredentialProviderUserArray_GetCount(This->Users, &Count);
        if (FAILED(hr))
            return hr;
        for (i = 0; i < Count && This->TileCount < PWPROVIDER_MAX_TILES; i++)
        {
            if (FAILED(ICredentialProviderUserArray_GetAt(This->Users, i, &User)))
                continue;
            hr = PasswordCredential_Create(This->Scenario, User, NULL, &Credential);
            ICredentialProviderUser_Release(User);
            if (SUCCEEDED(hr))
                This->Tiles[This->TileCount++] = Credential;
        }
        /* With accounts listed, the empty "Other user" tile comes only when the host asks for it. */
        if (This->TileCount &&
            (FAILED(ICredentialProviderUserArray_GetAccountOptions(This->Users, &Options)) ||
             !(Options & CPAO_EMPTY_LOCAL) || This->TileCount >= PWPROVIDER_MAX_TILES))
            return S_OK;
    }

    hr = PasswordCredential_Create(This->Scenario, NULL, This->Prefill[0] ? This->Prefill : NULL, &Credential);
    if (FAILED(hr))
        return hr;
    This->Tiles[This->TileCount++] = Credential;
    return S_OK;
}

static HRESULT WINAPI
Provider_QueryInterface(ICredentialProvider *iface, REFIID riid, void **ppv)
{
    PWPROVIDER *This = ProviderFrom(iface);

    if (!ppv)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProvider))
        *ppv = &This->ICredentialProvider_iface;
    else if (IsEqualGUID(riid, &IID_ICredentialProviderSetUserArray))
        *ppv = &This->ICredentialProviderSetUserArray_iface;
    else
    {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown *)*ppv);
    return S_OK;
}

static ULONG WINAPI
Provider_AddRef(ICredentialProvider *iface)
{
    return InterlockedIncrement(&ProviderFrom(iface)->Ref);
}

static ULONG WINAPI
Provider_Release(ICredentialProvider *iface)
{
    PWPROVIDER *This = ProviderFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);

    if (!Ref)
    {
        ReleaseTiles(This);
        if (This->Users)
            ICredentialProviderUserArray_Release(This->Users);
        if (This->Events)
            ICredentialProviderEvents_Release(This->Events);
        SecureZeroMemory(This, sizeof(*This));
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&CredProvsObjects);
    }
    return Ref;
}

static HRESULT WINAPI
Provider_SetUsageScenario(ICredentialProvider *iface, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags)
{
    PWPROVIDER *This = ProviderFrom(iface);

    TRACE("(%p, %d, %08lx)\n", This, cpus, dwFlags);

    if (!PasswordProvider_IsKnownScenario(cpus))
        return E_INVALIDARG;
    if (cpus == CPUS_CREDUI && (dwFlags & CREDUIWIN_PACK_32_WOW))
    {
        /* TODO: 32-bit packing for WOW64 CredUI callers. */
        FIXME("CREDUIWIN_PACK_32_WOW is not supported\n");
        return E_NOTIMPL;
    }
    This->Scenario = cpus;
    This->Flags = dwFlags;
    return BuildTiles(This);
}

static HRESULT WINAPI
Provider_SetSerialization(ICredentialProvider *iface, const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs)
{
    PWPROVIDER *This = ProviderFrom(iface);
    CREDSER_LOGON Logon;
    PVOID Copy;
    NTSTATUS Status;
    SIZE_T At = 0;

    TRACE("(%p, %p)\n", This, pcpcs);

    if (!pcpcs)
        return E_INVALIDARG;
    if (!IsEqualGUID(&pcpcs->clsidCredentialProvider, &CLSID_PasswordCredentialProvider) &&
        !IsEqualGUID(&pcpcs->clsidCredentialProvider, &GUID_NULL))
    {
        return E_INVALIDARG;
    }
    Status = CredSerCapture(pcpcs->rgbSerialization, pcpcs->cbSerialization, &Copy);
    if (!NT_SUCCESS(Status))
        return E_INVALIDARG;
    Status = CredSerUnpackLogon(Copy, pcpcs->cbSerialization, &Logon);
    if (NT_SUCCESS(Status))
    {
        if (Logon.LogonDomainName.Length)
        {
            CopyMemory(This->Prefill, Logon.LogonDomainName.Buffer, Logon.LogonDomainName.Length);
            At = Logon.LogonDomainName.Length / sizeof(WCHAR);
            This->Prefill[At++] = L'\\';
        }
        if (At + Logon.UserName.Length / sizeof(WCHAR) < PWFIELD_TEXT_CHARS)
        {
            CopyMemory(This->Prefill + At, Logon.UserName.Buffer, Logon.UserName.Length);
            This->Prefill[At + Logon.UserName.Length / sizeof(WCHAR)] = UNICODE_NULL;
        }
        else
        {
            This->Prefill[0] = UNICODE_NULL;
        }
    }
    CredSerFree(Copy, pcpcs->cbSerialization);
    if (!NT_SUCCESS(Status))
        return E_INVALIDARG;
    return This->Scenario ? BuildTiles(This) : S_OK;
}

static HRESULT WINAPI
Provider_Advise(ICredentialProvider *iface, ICredentialProviderEvents *pcpe, UINT_PTR upAdviseContext)
{
    PWPROVIDER *This = ProviderFrom(iface);

    if (!pcpe)
        return E_INVALIDARG;
    if (This->Events)
        ICredentialProviderEvents_Release(This->Events);
    This->Events = pcpe;
    ICredentialProviderEvents_AddRef(pcpe);
    This->AdviseContext = upAdviseContext;
    return S_OK;
}

static HRESULT WINAPI
Provider_UnAdvise(ICredentialProvider *iface)
{
    PWPROVIDER *This = ProviderFrom(iface);

    if (This->Events)
    {
        ICredentialProviderEvents_Release(This->Events);
        This->Events = NULL;
    }
    return S_OK;
}

static HRESULT WINAPI
Provider_GetFieldDescriptorCount(ICredentialProvider *iface, DWORD *pdwCount)
{
    if (!pdwCount)
        return E_POINTER;
    *pdwCount = ARRAYSIZE(FieldTable);
    return S_OK;
}

static HRESULT WINAPI
Provider_GetFieldDescriptorAt(ICredentialProvider *iface, DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd)
{
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Desc;
    WCHAR Label[64];
    HRESULT hr;

    if (!ppcpfd)
        return E_POINTER;
    *ppcpfd = NULL;
    if (dwIndex >= ARRAYSIZE(FieldTable))
        return E_INVALIDARG;

    Desc = CoTaskMemAlloc(sizeof(*Desc));
    if (!Desc)
        return E_OUTOFMEMORY;
    Desc->dwFieldID = FieldTable[dwIndex].Id;
    Desc->cpft = FieldTable[dwIndex].Type;
    Desc->guidFieldType = *FieldTable[dwIndex].FieldType;
    if (!LoadStringW(CredProvsInstance, FieldTable[dwIndex].Label, Label, ARRAYSIZE(Label)))
        Label[0] = UNICODE_NULL;
    hr = PasswordProvider_DupString(Label, &Desc->pszLabel);
    if (FAILED(hr))
    {
        CoTaskMemFree(Desc);
        return hr;
    }
    *ppcpfd = Desc;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetCredentialCount(ICredentialProvider *iface, DWORD *pdwCount, DWORD *pdwDefault, BOOL *pbAutoLogonWithDefault)
{
    PWPROVIDER *This = ProviderFrom(iface);

    if (!pdwCount || !pdwDefault || !pbAutoLogonWithDefault)
        return E_POINTER;
    *pdwCount = This->TileCount;
    *pdwDefault = (This->Scenario == CPUS_CREDUI) ? 0 : CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetCredentialAt(ICredentialProvider *iface, DWORD dwIndex, ICredentialProviderCredential **ppcpc)
{
    PWPROVIDER *This = ProviderFrom(iface);

    if (!ppcpc)
        return E_POINTER;
    *ppcpc = NULL;
    if (dwIndex >= This->TileCount)
        return E_INVALIDARG;
    return ICredentialProviderCredential2_QueryInterface(&This->Tiles[dwIndex]->ICredentialProviderCredential2_iface,
                                                         &IID_ICredentialProviderCredential, (void **)ppcpc);
}

static const ICredentialProviderVtbl ProviderVtbl =
{
    Provider_QueryInterface,
    Provider_AddRef,
    Provider_Release,
    Provider_SetUsageScenario,
    Provider_SetSerialization,
    Provider_Advise,
    Provider_UnAdvise,
    Provider_GetFieldDescriptorCount,
    Provider_GetFieldDescriptorAt,
    Provider_GetCredentialCount,
    Provider_GetCredentialAt,
};

static HRESULT WINAPI
SetUserArray_QueryInterface(ICredentialProviderSetUserArray *iface, REFIID riid, void **ppv)
{
    return Provider_QueryInterface(&ProviderFromSetUserArray(iface)->ICredentialProvider_iface, riid, ppv);
}

static ULONG WINAPI
SetUserArray_AddRef(ICredentialProviderSetUserArray *iface)
{
    return Provider_AddRef(&ProviderFromSetUserArray(iface)->ICredentialProvider_iface);
}

static ULONG WINAPI
SetUserArray_Release(ICredentialProviderSetUserArray *iface)
{
    return Provider_Release(&ProviderFromSetUserArray(iface)->ICredentialProvider_iface);
}

static HRESULT WINAPI
SetUserArray_SetUserArray(ICredentialProviderSetUserArray *iface, ICredentialProviderUserArray *users)
{
    PWPROVIDER *This = ProviderFromSetUserArray(iface);

    if (This->Users)
        ICredentialProviderUserArray_Release(This->Users);
    This->Users = users;
    if (users)
    {
        ICredentialProviderUserArray_AddRef(users);
        ICredentialProviderUserArray_SetProviderFilter(users, &LocalUserProvider);
    }
    return This->Scenario ? BuildTiles(This) : S_OK;
}

static const ICredentialProviderSetUserArrayVtbl SetUserArrayVtbl =
{
    SetUserArray_QueryInterface,
    SetUserArray_AddRef,
    SetUserArray_Release,
    SetUserArray_SetUserArray,
};

HRESULT
PasswordProvider_Create(_In_ REFIID riid, _Outptr_ void **ppv)
{
    PWPROVIDER *This;
    HRESULT hr;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProvider_iface.lpVtbl = &ProviderVtbl;
    This->ICredentialProviderSetUserArray_iface.lpVtbl = &SetUserArrayVtbl;
    This->Ref = 1;
    InterlockedIncrement(&CredProvsObjects);

    hr = Provider_QueryInterface(&This->ICredentialProvider_iface, riid, ppv);
    Provider_Release(&This->ICredentialProvider_iface);
    return hr;
}
