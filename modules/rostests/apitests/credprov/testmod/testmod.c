/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Third-party credential provider and filter used by the credprov framework tests
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <credentialprovider.h>
#include <initguid.h>
#include "testmod.h"

typedef struct _TEST_PROVIDER
{
    ICredentialProvider ICredentialProvider_iface;
    LONG Ref;
} TEST_PROVIDER;

typedef struct _TEST_FILTER
{
    ICredentialProviderFilter ICredentialProviderFilter_iface;
    LONG Ref;
} TEST_FILTER;

static HRESULT WINAPI
Provider_QueryInterface(ICredentialProvider *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProvider))
    {
        *ppv = iface;
        ICredentialProvider_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Provider_AddRef(ICredentialProvider *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, TEST_PROVIDER, ICredentialProvider_iface)->Ref);
}

static ULONG WINAPI
Provider_Release(ICredentialProvider *iface)
{
    TEST_PROVIDER *This = CONTAINING_RECORD(iface, TEST_PROVIDER, ICredentialProvider_iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
Provider_SetUsageScenario(ICredentialProvider *iface, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags)
{
    return (cpus == CPUS_LOGON || cpus == CPUS_UNLOCK_WORKSTATION) ? S_OK : E_NOTIMPL;
}

static HRESULT WINAPI
Provider_SetSerialization(ICredentialProvider *iface, const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI
Provider_Advise(ICredentialProvider *iface, ICredentialProviderEvents *pcpe, UINT_PTR upAdviseContext)
{
    return S_OK;
}

static HRESULT WINAPI
Provider_UnAdvise(ICredentialProvider *iface)
{
    return S_OK;
}

static HRESULT WINAPI
Provider_GetFieldDescriptorCount(ICredentialProvider *iface, DWORD *pdwCount)
{
    *pdwCount = 0;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetFieldDescriptorAt(ICredentialProvider *iface, DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd)
{
    *ppcpfd = NULL;
    return E_INVALIDARG;
}

static HRESULT WINAPI
Provider_GetCredentialCount(ICredentialProvider *iface, DWORD *pdwCount, DWORD *pdwDefault, BOOL *pbAutoLogonWithDefault)
{
    *pdwCount = 0;
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Provider_GetCredentialAt(ICredentialProvider *iface, DWORD dwIndex, ICredentialProviderCredential **ppcpc)
{
    *ppcpc = NULL;
    return E_INVALIDARG;
}

static const ICredentialProviderVtbl ProviderVtbl =
{
    Provider_QueryInterface, Provider_AddRef, Provider_Release, Provider_SetUsageScenario,
    Provider_SetSerialization, Provider_Advise, Provider_UnAdvise, Provider_GetFieldDescriptorCount,
    Provider_GetFieldDescriptorAt, Provider_GetCredentialCount, Provider_GetCredentialAt,
};

static HRESULT WINAPI
Filter_QueryInterface(ICredentialProviderFilter *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderFilter))
    {
        *ppv = iface;
        ICredentialProviderFilter_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Filter_AddRef(ICredentialProviderFilter *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, TEST_FILTER, ICredentialProviderFilter_iface)->Ref);
}

static ULONG WINAPI
Filter_Release(ICredentialProviderFilter *iface)
{
    TEST_FILTER *This = CONTAINING_RECORD(iface, TEST_FILTER, ICredentialProviderFilter_iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
Filter_Filter(ICredentialProviderFilter *iface, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags,
              GUID *rgclsidProviders, BOOL *rgbAllow, DWORD cProviders)
{
    DWORD i;

    for (i = 0; i < cProviders; i++)
    {
        if (IsEqualGUID(&rgclsidProviders[i], &CLSID_CredProvTestProvider))
            rgbAllow[i] = FALSE;
    }
    return S_OK;
}

static HRESULT WINAPI
Filter_UpdateRemoteCredential(ICredentialProviderFilter *iface,
                              const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcsIn,
                              CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcsOut)
{
    *pcpcsOut = *pcpcsIn;
    pcpcsOut->rgbSerialization = NULL;
    pcpcsOut->cbSerialization = 0;
    pcpcsOut->clsidCredentialProvider = CLSID_CredProvTestProvider;
    return S_OK;
}

static const ICredentialProviderFilterVtbl FilterVtbl =
{
    Filter_QueryInterface, Filter_AddRef, Filter_Release, Filter_Filter, Filter_UpdateRemoteCredential,
};

static HRESULT WINAPI
Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI
Factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI
ProviderFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    TEST_PROVIDER *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProvider_iface.lpVtbl = &ProviderVtbl;
    This->Ref = 1;
    hr = ICredentialProvider_QueryInterface(&This->ICredentialProvider_iface, riid, ppv);
    ICredentialProvider_Release(&This->ICredentialProvider_iface);
    return hr;
}

static HRESULT WINAPI
FilterFactory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    TEST_FILTER *This;
    HRESULT hr;

    *ppv = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProviderFilter_iface.lpVtbl = &FilterVtbl;
    This->Ref = 1;
    hr = ICredentialProviderFilter_QueryInterface(&This->ICredentialProviderFilter_iface, riid, ppv);
    ICredentialProviderFilter_Release(&This->ICredentialProviderFilter_iface);
    return hr;
}

static HRESULT WINAPI
Factory_LockServer(IClassFactory *iface, BOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl ProviderFactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, ProviderFactory_CreateInstance, Factory_LockServer,
};

static const IClassFactoryVtbl FilterFactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, FilterFactory_CreateInstance, Factory_LockServer,
};

static IClassFactory ProviderFactory = { &ProviderFactoryVtbl };
static IClassFactory FilterFactory = { &FilterFactoryVtbl };

HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (IsEqualGUID(clsid, &CLSID_CredProvTestProvider) || IsEqualGUID(clsid, &CLSID_CredProvTestDependent))
        return IClassFactory_QueryInterface(&ProviderFactory, riid, ppv);
    if (IsEqualGUID(clsid, &CLSID_CredProvTestFilter))
        return IClassFactory_QueryInterface(&FilterFactory, riid, ppv);
    *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI
DllCanUnloadNow(void)
{
    return S_FALSE;
}
